#include "AvroRowInputFormat.h"
#if USE_AVRO

#include <numeric>

#include <Core/Defines.h>
#include <Core/Field.h>

#include <IO/Operators.h>
#include <IO/ReadHelpers.h>
#include <IO/HTTPCommon.h>

#include <Formats/verbosePrintString.h>
#include <Formats/FormatFactory.h>

#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <DataTypes/DataTypeEnum.h>
#include <DataTypes/DataTypeFixedString.h>
#include <DataTypes/DataTypeNothing.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypeTuple.h>
#include <DataTypes/IDataType.h>
#include <DataTypes/getLeastSupertype.h>

#include <Columns/ColumnArray.h>
#include <Columns/ColumnFixedString.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnsNumber.h>

#include <avro/Compiler.hh>
#include <avro/DataFile.hh>
#include <avro/Decoder.hh>
#include <avro/Encoder.hh>
#include <avro/Generic.hh>
#include <avro/GenericDatum.hh>
#include <avro/Node.hh>
#include <avro/NodeConcepts.hh>
#include <avro/NodeImpl.hh>
#include <avro/Reader.hh>
#include <avro/Schema.hh>
#include <avro/Specific.hh>
#include <avro/Types.hh>
#include <avro/ValidSchema.hh>
#include <avro/Writer.hh>

#include <Poco/BinaryReader.h>
#include <Poco/Buffer.h>
#include <Poco/JSON/JSON.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/MemoryStream.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Poco.h>
#include <Poco/URI.h>


namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int THERE_IS_NO_COLUMN;
    extern const int INCORRECT_DATA;
    extern const int ILLEGAL_COLUMN;
    extern const int TYPE_MISMATCH;
}

class InputStreamReadBufferAdapter : public avro::InputStream
{
public:
    InputStreamReadBufferAdapter(ReadBuffer & in_) : in(in_) {}

    bool next(const uint8_t ** data, size_t * len) override
    {
        if (in.eof())
        {
            *len = 0;
            return false;
        }

        *data = reinterpret_cast<const uint8_t *>(in.position());
        *len = in.available();

        in.position() += in.available();
        return true;
    }

    void backup(size_t len) override { in.position() -= len; }

    void skip(size_t len) override { in.tryIgnore(len); }

    size_t byteCount() const override { return in.count(); }

private:
    ReadBuffer & in;
};

static void deserializeNoop(IColumn &, avro::Decoder &)
{
}

/// Insert value with conversion to the column of target type.
template <typename T>
static void insertNumber(IColumn & column, WhichDataType type, T value)
{
    switch (type.idx)
    {
        case TypeIndex::UInt8:
            assert_cast<ColumnUInt8 &>(column).insertValue(value);
            break;
        case TypeIndex::Date: [[fallthrough]];
        case TypeIndex::UInt16:
            assert_cast<ColumnUInt16 &>(column).insertValue(value);
            break;
        case TypeIndex::DateTime: [[fallthrough]];
        case TypeIndex::UInt32:
            assert_cast<ColumnUInt32 &>(column).insertValue(value);
            break;
        case TypeIndex::DateTime64: [[fallthrough]];
        case TypeIndex::UInt64:
            assert_cast<ColumnUInt64 &>(column).insertValue(value);
            break;
        case TypeIndex::Int8:
            assert_cast<ColumnInt8 &>(column).insertValue(value);
            break;
        case TypeIndex::Int16:
            assert_cast<ColumnInt16 &>(column).insertValue(value);
            break;
        case TypeIndex::Int32:
            assert_cast<ColumnInt32 &>(column).insertValue(value);
            break;
        case TypeIndex::Int64:
            assert_cast<ColumnInt64 &>(column).insertValue(value);
            break;
        case TypeIndex::Float32:
            assert_cast<ColumnFloat32 &>(column).insertValue(value);
            break;
        case TypeIndex::Float64:
            assert_cast<ColumnFloat64 &>(column).insertValue(value);
            break;
        default:
            throw Exception("Type is not compatible with Avro", ErrorCodes::ILLEGAL_COLUMN);
    }
}

static std::string nodeToJson(avro::NodePtr root_node)
{
    std::ostringstream ss;
    root_node->printJson(ss, 0);
    return ss.str();
}

AvroDeserializer::DeserializeFn AvroDeserializer::createDeserializeFn(avro::NodePtr root_node, DataTypePtr target_type)
{
    WhichDataType target(target_type);
    switch (root_node->type())
    {
        case avro::AVRO_STRING: [[fallthrough]];
        case avro::AVRO_BYTES:
            if (target.isString() || target.isFixedString())
            {
                return [tmp = std::string()](IColumn & column, avro::Decoder & decoder) mutable
                {
                    decoder.decodeString(tmp);
                    column.insertData(tmp.c_str(), tmp.length());
                };
            }
            break;
        case avro::AVRO_INT:
            return [target](IColumn & column, avro::Decoder & decoder)
            {
                insertNumber(column, target, decoder.decodeInt());
            };
        case avro::AVRO_LONG:
            if (target.isDateTime64())
            {
                auto date_time_scale = assert_cast<const DataTypeDateTime64 &>(*target_type).getScale();
                auto logical_type = root_node->logicalType().type();
                if ((logical_type == avro::LogicalType::TIMESTAMP_MILLIS && date_time_scale == 3)
                    || (logical_type == avro::LogicalType::TIMESTAMP_MICROS && date_time_scale == 6))
                {
                    return [](IColumn & column, avro::Decoder & decoder)
                    {
                        assert_cast<DataTypeDateTime64::ColumnType &>(column).insertValue(decoder.decodeLong());
                    };
                }
            }
            else
            {
                return [target](IColumn & column, avro::Decoder & decoder)
                {
                    insertNumber(column, target, decoder.decodeLong());
                };
            }
            break;
        case avro::AVRO_FLOAT:
            return [target](IColumn & column, avro::Decoder & decoder)
            {
                insertNumber(column, target, decoder.decodeFloat());
            };
        case avro::AVRO_DOUBLE:
            return [target](IColumn & column, avro::Decoder & decoder)
            {
                insertNumber(column, target, decoder.decodeDouble());
            };
        case avro::AVRO_BOOL:
            return [target](IColumn & column, avro::Decoder & decoder)
            {
                insertNumber(column, target, decoder.decodeBool());
            };
        case avro::AVRO_ARRAY:
            if (target.isArray())
            {
                auto nested_source_type = root_node->leafAt(0);
                auto nested_target_type = assert_cast<const DataTypeArray &>(*target_type).getNestedType();
                auto nested_deserialize = createDeserializeFn(nested_source_type, nested_target_type);
                return [nested_deserialize](IColumn & column, avro::Decoder & decoder)
                {
                    ColumnArray & column_array = assert_cast<ColumnArray &>(column);
                    ColumnArray::Offsets & offsets = column_array.getOffsets();
                    IColumn & nested_column = column_array.getData();
                    size_t total = 0;
                    for (size_t n = decoder.arrayStart(); n != 0; n = decoder.arrayNext())
                    {
                        total += n;
                        for (size_t i = 0; i < n; i++)
                        {
                            nested_deserialize(nested_column, decoder);
                        }
                    }
                    offsets.push_back(offsets.back() + total);
                };
            }
            break;
        case avro::AVRO_UNION:
        {
            auto nullable_deserializer = [root_node, target_type](size_t non_null_union_index)
            {
                auto nested_deserialize = createDeserializeFn(root_node->leafAt(non_null_union_index), removeNullable(target_type));
                return [non_null_union_index, nested_deserialize](IColumn & column, avro::Decoder & decoder)
                {
                    ColumnNullable & col = assert_cast<ColumnNullable &>(column);
                    size_t union_index = decoder.decodeUnionIndex();
                    if (union_index == non_null_union_index)
                    {
                        nested_deserialize(col.getNestedColumn(), decoder);
                        col.getNullMapData().push_back(0);
                    }
                    else
                    {
                        col.insertDefault();
                    }
                };
            };
            if (root_node->leaves() == 2 && target.isNullable())
            {
                if (root_node->leafAt(0)->type() == avro::AVRO_NULL)
                    return nullable_deserializer(1);
                if (root_node->leafAt(1)->type() == avro::AVRO_NULL)
                    return nullable_deserializer(0);
            }
            break;
        }
        case avro::AVRO_NULL:
            if (target.isNullable())
            {
                auto nested_type = removeNullable(target_type);
                if (nested_type->getTypeId() == TypeIndex::Nothing)
                {
                    return [](IColumn &, avro::Decoder & decoder)
                    {
                        decoder.decodeNull();
                    };
                }
                else
                {
                    return [](IColumn & column, avro::Decoder & decoder)
                    {
                        ColumnNullable & col = assert_cast<ColumnNullable &>(column);
                        decoder.decodeNull();
                        col.insertDefault();
                    };
                }
            }
            break;
        case avro::AVRO_ENUM:
            if (target.isString())
            {
                std::vector<std::string> symbols;
                for (size_t i = 0; i < root_node->names(); i++)
                {
                    symbols.push_back(root_node->nameAt(i));
                }
                return [symbols](IColumn & column, avro::Decoder & decoder)
                {
                    size_t enum_index = decoder.decodeEnum();
                    const auto & enum_symbol = symbols[enum_index];
                    column.insertData(enum_symbol.c_str(), enum_symbol.length());
                };
            }
            if (target.isEnum())
            {
                const auto & enum_type = dynamic_cast<const IDataTypeEnum &>(*target_type);
                std::vector<Field> symbol_mapping;
                for (size_t i = 0; i < root_node->names(); i++)
                {
                    symbol_mapping.push_back(enum_type.castToValue(root_node->nameAt(i)));
                }
                return [symbol_mapping](IColumn & column, avro::Decoder & decoder)
                {
                    size_t enum_index = decoder.decodeEnum();
                    column.insert(symbol_mapping[enum_index]);
                };
            }
            break;
        case avro::AVRO_FIXED:
        {
            size_t fixed_size = root_node->fixedSize();
            if ((target.isFixedString() && target_type->getSizeOfValueInMemory() == fixed_size) || target.isString())
            {
                return [tmp_fixed = std::vector<uint8_t>(fixed_size)](IColumn & column, avro::Decoder & decoder) mutable
                {
                    decoder.decodeFixed(tmp_fixed.size(), tmp_fixed);
                    column.insertData(reinterpret_cast<const char *>(tmp_fixed.data()), tmp_fixed.size());
                };
            }
            break;
        }
        case avro::AVRO_SYMBOLIC:
            return createDeserializeFn(avro::resolveSymbol(root_node), target_type);
        case avro::AVRO_MAP: [[fallthrough]];
        case avro::AVRO_RECORD: [[fallthrough]];
        default:
            break;
    }

    throw Exception(
        "Type " + target_type->getName() + " is not compatible with Avro " + avro::toString(root_node->type()) + ":\n" + nodeToJson(root_node),
        ErrorCodes::ILLEGAL_COLUMN);
}

AvroDeserializer::SkipFn AvroDeserializer::createSkipFn(avro::NodePtr root_node)
{
    switch (root_node->type())
    {
        case avro::AVRO_STRING:
            return [](avro::Decoder & decoder) { decoder.skipString(); };
        case avro::AVRO_BYTES:
            return [](avro::Decoder & decoder) { decoder.skipBytes(); };
        case avro::AVRO_INT:
            return [](avro::Decoder & decoder) { decoder.decodeInt(); };
        case avro::AVRO_LONG:
            return [](avro::Decoder & decoder) { decoder.decodeLong(); };
        case avro::AVRO_FLOAT:
            return [](avro::Decoder & decoder) { decoder.decodeFloat(); };
        case avro::AVRO_DOUBLE:
            return [](avro::Decoder & decoder) { decoder.decodeDouble(); };
        case avro::AVRO_BOOL:
            return [](avro::Decoder & decoder) { decoder.decodeBool(); };
        case avro::AVRO_ARRAY:
        {
            auto nested_skip_fn = createSkipFn(root_node->leafAt(0));
            return [nested_skip_fn](avro::Decoder & decoder)
            {
                for (size_t n = decoder.arrayStart(); n != 0; n = decoder.arrayNext())
                {
                    for (size_t i = 0; i < n; ++i)
                    {
                        nested_skip_fn(decoder);
                    }
                }
            };
        }
        case avro::AVRO_UNION:
        {
            std::vector<SkipFn> union_skip_fns;
            for (size_t i = 0; i < root_node->leaves(); i++)
            {
                union_skip_fns.push_back(createSkipFn(root_node->leafAt(i)));
            }
            return [union_skip_fns](avro::Decoder & decoder) { union_skip_fns[decoder.decodeUnionIndex()](decoder); };
        }
        case avro::AVRO_NULL:
            return [](avro::Decoder & decoder) { decoder.decodeNull(); };
        case avro::AVRO_ENUM:
            return [](avro::Decoder & decoder) { decoder.decodeEnum(); };
        case avro::AVRO_FIXED:
        {
            auto fixed_size = root_node->fixedSize();
            return [fixed_size](avro::Decoder & decoder) { decoder.skipFixed(fixed_size); };
        }
        case avro::AVRO_MAP:
        {
            auto value_skip_fn = createSkipFn(root_node->leafAt(1));
            return [value_skip_fn](avro::Decoder & decoder)
            {
                for (size_t n = decoder.mapStart(); n != 0; n = decoder.mapNext())
                {
                    for (size_t i = 0; i < n; ++i)
                    {
                        decoder.skipString();
                        value_skip_fn(decoder);
                    }
                }
            };
        }
        case avro::AVRO_RECORD:
        {
            std::vector<SkipFn> field_skip_fns;
            for (size_t i = 0; i < root_node->leaves(); i++)
            {
                field_skip_fns.push_back(createSkipFn(root_node->leafAt(i)));
            }
            return [field_skip_fns](avro::Decoder & decoder)
            {
                for (auto & skip_fn : field_skip_fns)
                    skip_fn(decoder);
            };
        }
        case avro::AVRO_SYMBOLIC:
        {
            auto [it, inserted] = symbolic_skip_fn_map.emplace(root_node->name(), SkipFn{});
            if (inserted)
            {
                it->second = createSkipFn(avro::resolveSymbol(root_node));
            }
            return [&skip_fn = it->second](avro::Decoder & decoder)
            {
                skip_fn(decoder);
            };
        }
        default:
            throw Exception("Unsupported Avro type " + root_node->name().fullname() + " (" + toString(int(root_node->type())) + ")", ErrorCodes::ILLEGAL_COLUMN);
    }
}


AvroDeserializer::AvroDeserializer(const ColumnsWithTypeAndName & columns, avro::ValidSchema schema)
{
    auto schema_root = schema.root();
    if (schema_root->type() != avro::AVRO_RECORD)
    {
        throw Exception("Root schema must be a record", ErrorCodes::TYPE_MISMATCH);
    }

    field_mapping.resize(schema_root->leaves(), -1);

    for (size_t i = 0; i < schema_root->leaves(); ++i)
    {
        skip_fns.push_back(createSkipFn(schema_root->leafAt(i)));
        deserialize_fns.push_back(&deserializeNoop);
    }

    for (size_t i = 0; i < columns.size(); ++i)
    {
        const auto & column = columns[i];
        size_t field_index = 0;
        if (!schema_root->nameIndex(column.name, field_index))
        {
            throw Exception("Field " + column.name + " not found in Avro schema", ErrorCodes::THERE_IS_NO_COLUMN);
        }
        auto field_schema = schema_root->leafAt(field_index);
        try
        {
            deserialize_fns[field_index] = createDeserializeFn(field_schema, column.type);
        }
        catch (Exception & e)
        {
            e.addMessage("column " + column.name);
            throw;
        }
        field_mapping[field_index] = i;
    }
}

void AvroDeserializer::deserializeRow(MutableColumns & columns, avro::Decoder & decoder) const
{
    for (size_t i = 0; i < field_mapping.size(); i++)
    {
        if (field_mapping[i] >= 0)
        {
            deserialize_fns[i](*columns[field_mapping[i]], decoder);
        }
        else
        {
            skip_fns[i](decoder);
        }
    }
}


AvroRowInputFormat::AvroRowInputFormat(const Block & header_, ReadBuffer & in_, Params params_)
    : IRowInputFormat(header_, in_, params_)
    , file_reader(std::make_unique<InputStreamReadBufferAdapter>(in_))
    , deserializer(header_.getColumnsWithTypeAndName(), file_reader.dataSchema())
{
    file_reader.init();
}

bool AvroRowInputFormat::readRow(MutableColumns & columns, RowReadExtension &)
{
    if (file_reader.hasMore())
    {
        file_reader.decr();
        deserializer.deserializeRow(columns, file_reader.decoder());
        return true;
    }
    return false;
}

#if USE_POCO_JSON
class AvroConfluentRowInputFormat::SchemaRegistry
{
public:
    SchemaRegistry(const std::string & base_url_, size_t schema_cache_max_size = 1000)
        : base_url(base_url_), schema_cache(schema_cache_max_size)
    {
        if (base_url.empty())
            throw Exception("Empty Schema Registry URL", ErrorCodes::BAD_ARGUMENTS);
    }

    avro::ValidSchema getSchema(uint32_t id)
    {
        auto [schema, loaded] = schema_cache.getOrSet(
            id,
            [this, id](){ return std::make_shared<avro::ValidSchema>(fetchSchema(id)); }
        );
        return *schema;
    }

private:
    avro::ValidSchema fetchSchema(uint32_t id)
    {
        try
        {
            try
            {
                Poco::URI url(base_url, "/schemas/ids/" + std::to_string(id));
                LOG_TRACE((&Logger::get("AvroConfluentRowInputFormat")), "Fetching schema id = " << id);

                /// One second for connect/send/receive. Just in case.
                ConnectionTimeouts timeouts({1, 0}, {1, 0}, {1, 0});

                Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_GET, url.getPathAndQuery(), Poco::Net::HTTPRequest::HTTP_1_1);
                request.setHost(url.getHost());

                auto session = makePooledHTTPSession(url, timeouts, 1);
                session->sendRequest(request);

                Poco::Net::HTTPResponse response;
                auto response_body = receiveResponse(*session, request, response, false);

                Poco::JSON::Parser parser;
                auto json_body = parser.parse(*response_body).extract<Poco::JSON::Object::Ptr>();
                auto schema = json_body->getValue<std::string>("schema");
                LOG_TRACE((&Logger::get("AvroConfluentRowInputFormat")),
                    "Succesfully fetched schema  id = " << id << "\n" << schema);
                return avro::compileJsonSchemaFromString(schema);
            }
            catch (const Exception &)
            {
                throw;
            }
            catch (const Poco::Exception & e)
            {
                throw Exception(Exception::CreateFromPoco, e);
            }
            catch (const avro::Exception & e)
            {
                throw Exception(e.what(), ErrorCodes::INCORRECT_DATA);
            }
        }
        catch (Exception & e)
        {
            e.addMessage("while fetching schema id = " + std::to_string(id));
            throw;
        }
    }

private:
    Poco::URI base_url;
    LRUCache<uint32_t, avro::ValidSchema> schema_cache;
};

using ConfluentSchemaRegistry = AvroConfluentRowInputFormat::SchemaRegistry;
#define SCHEMA_REGISTRY_CACHE_MAX_SIZE 1000
/// Cache of Schema Registry URL -> SchemaRegistry
static LRUCache<std::string, ConfluentSchemaRegistry>  schema_registry_cache(SCHEMA_REGISTRY_CACHE_MAX_SIZE);

static std::shared_ptr<ConfluentSchemaRegistry> getConfluentSchemaRegistry(const FormatSettings & format_settings)
{
    const auto & base_url = format_settings.avro.schema_registry_url;
    auto [schema_registry, loaded] = schema_registry_cache.getOrSet(
        base_url,
        [base_url]()
        {
            return std::make_shared<ConfluentSchemaRegistry>(base_url);
        }
    );
    return schema_registry;
}

static uint32_t readConfluentSchemaId(ReadBuffer & in)
{
    uint8_t magic;
    uint32_t schema_id;

    readBinaryBigEndian(magic, in);
    readBinaryBigEndian(schema_id, in);

    if (magic != 0x00)
    {
        throw Exception("Invalid magic byte before AvroConfluent schema identifier."
            " Must be zero byte, found " + std::to_string(int(magic)) + " instead", ErrorCodes::INCORRECT_DATA);
    }

    return schema_id;
}

AvroConfluentRowInputFormat::AvroConfluentRowInputFormat(
    const Block & header_, ReadBuffer & in_, Params params_, const FormatSettings & format_settings_)
    : IRowInputFormat(header_.cloneEmpty(), in_, params_)
    , header_columns(header_.getColumnsWithTypeAndName())
    , schema_registry(getConfluentSchemaRegistry(format_settings_))
    , input_stream(std::make_unique<InputStreamReadBufferAdapter>(in))
    , decoder(avro::binaryDecoder())

{
    decoder->init(*input_stream);
}

bool AvroConfluentRowInputFormat::readRow(MutableColumns & columns, RowReadExtension &)
{
    if (in.eof())
    {
        return false;
    }
    SchemaId schema_id = readConfluentSchemaId(in);
    auto & deserializer = getOrCreateDeserializer(schema_id);
    deserializer.deserializeRow(columns, *decoder);
    decoder->drain();
    return true;
}

const AvroDeserializer & AvroConfluentRowInputFormat::getOrCreateDeserializer(SchemaId schema_id)
{
    auto it = deserializer_cache.find(schema_id);
    if (it == deserializer_cache.end())
    {
        auto schema = schema_registry->getSchema(schema_id);
        AvroDeserializer deserializer(header_columns, schema);
        it = deserializer_cache.emplace(schema_id, deserializer).first;
    }
    return it->second;
}
#endif

void registerInputFormatProcessorAvro(FormatFactory & factory)
{
    factory.registerInputFormatProcessor("Avro", [](
        ReadBuffer & buf,
        const Block & sample,
        const RowInputFormatParams & params,
        const FormatSettings &)
    {
        return std::make_shared<AvroRowInputFormat>(sample, buf, params);
    });

#if USE_POCO_JSON
    factory.registerInputFormatProcessor("AvroConfluent",[](
        ReadBuffer & buf,
        const Block & sample,
        const RowInputFormatParams & params,
        const FormatSettings & settings)
    {
        return std::make_shared<AvroConfluentRowInputFormat>(sample, buf, params, settings);
    });
#endif

}

}

#else

namespace DB
{
class FormatFactory;
void registerInputFormatProcessorAvro(FormatFactory &)
{
}
}

#endif
