// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "arrow/ipc/metadata.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "flatbuffers/flatbuffers.h"

#include "arrow/array.h"
#include "arrow/buffer.h"
#include "arrow/io/interfaces.h"
#include "arrow/ipc/File_generated.h"
#include "arrow/ipc/Message_generated.h"
#include "arrow/ipc/Tensor_generated.h"
#include "arrow/ipc/util.h"
#include "arrow/status.h"
#include "arrow/tensor.h"
#include "arrow/type.h"

namespace arrow {

namespace flatbuf = org::apache::arrow::flatbuf;

namespace ipc {

using FBB = flatbuffers::FlatBufferBuilder;
using DictionaryOffset = flatbuffers::Offset<flatbuf::DictionaryEncoding>;
using FieldOffset = flatbuffers::Offset<flatbuf::Field>;
using KeyValueOffset = flatbuffers::Offset<flatbuf::KeyValue>;
using RecordBatchOffset = flatbuffers::Offset<flatbuf::RecordBatch>;
using VectorLayoutOffset = flatbuffers::Offset<arrow::flatbuf::VectorLayout>;
using Offset = flatbuffers::Offset<void>;
using FBString = flatbuffers::Offset<flatbuffers::String>;

static constexpr flatbuf::MetadataVersion kCurrentMetadataVersion =
    flatbuf::MetadataVersion_V3;

static constexpr flatbuf::MetadataVersion kMinMetadataVersion =
    flatbuf::MetadataVersion_V3;

static Status IntFromFlatbuffer(const flatbuf::Int* int_data,
                                std::shared_ptr<DataType>* out) {
  if (int_data->bitWidth() > 64) {
    return Status::NotImplemented("Integers with more than 64 bits not implemented");
  }
  if (int_data->bitWidth() < 8) {
    return Status::NotImplemented("Integers with less than 8 bits not implemented");
  }

  switch (int_data->bitWidth()) {
    case 8:
      *out = int_data->is_signed() ? int8() : uint8();
      break;
    case 16:
      *out = int_data->is_signed() ? int16() : uint16();
      break;
    case 32:
      *out = int_data->is_signed() ? int32() : uint32();
      break;
    case 64:
      *out = int_data->is_signed() ? int64() : uint64();
      break;
    default:
      return Status::NotImplemented("Integers not in cstdint are not implemented");
  }
  return Status::OK();
}

static Status FloatFromFlatuffer(const flatbuf::FloatingPoint* float_data,
                                 std::shared_ptr<DataType>* out) {
  if (float_data->precision() == flatbuf::Precision_HALF) {
    *out = float16();
  } else if (float_data->precision() == flatbuf::Precision_SINGLE) {
    *out = float32();
  } else {
    *out = float64();
  }
  return Status::OK();
}

// Forward declaration
static Status FieldToFlatbuffer(FBB& fbb, const std::shared_ptr<Field>& field,
                                DictionaryMemo* dictionary_memo, FieldOffset* offset);

static Offset IntToFlatbuffer(FBB& fbb, int bitWidth, bool is_signed) {
  return flatbuf::CreateInt(fbb, bitWidth, is_signed).Union();
}

static Offset FloatToFlatbuffer(FBB& fbb, flatbuf::Precision precision) {
  return flatbuf::CreateFloatingPoint(fbb, precision).Union();
}

static Status AppendChildFields(FBB& fbb, const std::shared_ptr<DataType>& type,
                                std::vector<FieldOffset>* out_children,
                                DictionaryMemo* dictionary_memo) {
  FieldOffset field;
  for (int i = 0; i < type->num_children(); ++i) {
    RETURN_NOT_OK(FieldToFlatbuffer(fbb, type->child(i), dictionary_memo, &field));
    out_children->push_back(field);
  }
  return Status::OK();
}

static Status ListToFlatbuffer(FBB& fbb, const std::shared_ptr<DataType>& type,
                               std::vector<FieldOffset>* out_children,
                               DictionaryMemo* dictionary_memo, Offset* offset) {
  RETURN_NOT_OK(AppendChildFields(fbb, type, out_children, dictionary_memo));
  *offset = flatbuf::CreateList(fbb).Union();
  return Status::OK();
}

static Status StructToFlatbuffer(FBB& fbb, const std::shared_ptr<DataType>& type,
                                 std::vector<FieldOffset>* out_children,
                                 DictionaryMemo* dictionary_memo, Offset* offset) {
  RETURN_NOT_OK(AppendChildFields(fbb, type, out_children, dictionary_memo));
  *offset = flatbuf::CreateStruct_(fbb).Union();
  return Status::OK();
}

// ----------------------------------------------------------------------
// Union implementation

static Status UnionFromFlatbuffer(const flatbuf::Union* union_data,
                                  const std::vector<std::shared_ptr<Field>>& children,
                                  std::shared_ptr<DataType>* out) {
  UnionMode mode = union_data->mode() == flatbuf::UnionMode_Sparse ? UnionMode::SPARSE
                                                                   : UnionMode::DENSE;

  std::vector<uint8_t> type_codes;

  const flatbuffers::Vector<int32_t>* fb_type_ids = union_data->typeIds();
  if (fb_type_ids == nullptr) {
    for (uint8_t i = 0; i < children.size(); ++i) {
      type_codes.push_back(i);
    }
  } else {
    for (int32_t id : (*fb_type_ids)) {
      // TODO(wesm): can these values exceed 255?
      type_codes.push_back(static_cast<uint8_t>(id));
    }
  }

  *out = union_(children, type_codes, mode);
  return Status::OK();
}

static Status UnionToFlatBuffer(FBB& fbb, const std::shared_ptr<DataType>& type,
                                std::vector<FieldOffset>* out_children,
                                DictionaryMemo* dictionary_memo, Offset* offset) {
  RETURN_NOT_OK(AppendChildFields(fbb, type, out_children, dictionary_memo));

  const auto& union_type = static_cast<const UnionType&>(*type);

  flatbuf::UnionMode mode = union_type.mode() == UnionMode::SPARSE
                                ? flatbuf::UnionMode_Sparse
                                : flatbuf::UnionMode_Dense;

  std::vector<int32_t> type_ids;
  type_ids.reserve(union_type.type_codes().size());
  for (uint8_t code : union_type.type_codes()) {
    type_ids.push_back(code);
  }

  auto fb_type_ids = fbb.CreateVector(type_ids);

  *offset = flatbuf::CreateUnion(fbb, mode, fb_type_ids).Union();
  return Status::OK();
}

#define INT_TO_FB_CASE(BIT_WIDTH, IS_SIGNED)            \
  *out_type = flatbuf::Type_Int;                        \
  *offset = IntToFlatbuffer(fbb, BIT_WIDTH, IS_SIGNED); \
  break;

static inline flatbuf::TimeUnit ToFlatbufferUnit(TimeUnit::type unit) {
  switch (unit) {
    case TimeUnit::SECOND:
      return flatbuf::TimeUnit_SECOND;
    case TimeUnit::MILLI:
      return flatbuf::TimeUnit_MILLISECOND;
    case TimeUnit::MICRO:
      return flatbuf::TimeUnit_MICROSECOND;
    case TimeUnit::NANO:
      return flatbuf::TimeUnit_NANOSECOND;
    default:
      break;
  }
  return flatbuf::TimeUnit_MIN;
}

static inline TimeUnit::type FromFlatbufferUnit(flatbuf::TimeUnit unit) {
  switch (unit) {
    case flatbuf::TimeUnit_SECOND:
      return TimeUnit::SECOND;
    case flatbuf::TimeUnit_MILLISECOND:
      return TimeUnit::MILLI;
    case flatbuf::TimeUnit_MICROSECOND:
      return TimeUnit::MICRO;
    case flatbuf::TimeUnit_NANOSECOND:
      return TimeUnit::NANO;
    default:
      break;
  }
  // cannot reach
  return TimeUnit::SECOND;
}

static Status TypeFromFlatbuffer(flatbuf::Type type, const void* type_data,
                                 const std::vector<std::shared_ptr<Field>>& children,
                                 std::shared_ptr<DataType>* out) {
  switch (type) {
    case flatbuf::Type_NONE:
      return Status::Invalid("Type metadata cannot be none");
    case flatbuf::Type_Int:
      return IntFromFlatbuffer(static_cast<const flatbuf::Int*>(type_data), out);
    case flatbuf::Type_FloatingPoint:
      return FloatFromFlatuffer(static_cast<const flatbuf::FloatingPoint*>(type_data),
                                out);
    case flatbuf::Type_Binary:
      *out = binary();
      return Status::OK();
    case flatbuf::Type_FixedSizeBinary: {
      auto fw_binary = static_cast<const flatbuf::FixedSizeBinary*>(type_data);
      *out = fixed_size_binary(fw_binary->byteWidth());
      return Status::OK();
    }
    case flatbuf::Type_Utf8:
      *out = utf8();
      return Status::OK();
    case flatbuf::Type_Bool:
      *out = boolean();
      return Status::OK();
    case flatbuf::Type_Decimal:
      return Status::NotImplemented("Decimal");
    case flatbuf::Type_Date: {
      auto date_type = static_cast<const flatbuf::Date*>(type_data);
      if (date_type->unit() == flatbuf::DateUnit_DAY) {
        *out = date32();
      } else {
        *out = date64();
      }
      return Status::OK();
    }
    case flatbuf::Type_Time: {
      auto time_type = static_cast<const flatbuf::Time*>(type_data);
      TimeUnit::type unit = FromFlatbufferUnit(time_type->unit());
      int32_t bit_width = time_type->bitWidth();
      switch (unit) {
        case TimeUnit::SECOND:
        case TimeUnit::MILLI:
          if (bit_width != 32) {
            return Status::Invalid("Time is 32 bits for second/milli unit");
          }
          *out = time32(unit);
          break;
        default:
          if (bit_width != 64) {
            return Status::Invalid("Time is 64 bits for micro/nano unit");
          }
          *out = time64(unit);
          break;
      }
      return Status::OK();
    }
    case flatbuf::Type_Timestamp: {
      auto ts_type = static_cast<const flatbuf::Timestamp*>(type_data);
      TimeUnit::type unit = FromFlatbufferUnit(ts_type->unit());
      if (ts_type->timezone() != 0 && ts_type->timezone()->Length() > 0) {
        *out = timestamp(unit, ts_type->timezone()->str());
      } else {
        *out = timestamp(unit);
      }
      return Status::OK();
    }
    case flatbuf::Type_Interval:
      return Status::NotImplemented("Interval");
    case flatbuf::Type_List:
      if (children.size() != 1) {
        return Status::Invalid("List must have exactly 1 child field");
      }
      *out = std::make_shared<ListType>(children[0]);
      return Status::OK();
    case flatbuf::Type_Struct_:
      *out = std::make_shared<StructType>(children);
      return Status::OK();
    case flatbuf::Type_Union:
      return UnionFromFlatbuffer(static_cast<const flatbuf::Union*>(type_data), children,
                                 out);
    default:
      return Status::Invalid("Unrecognized type");
  }
}

// TODO(wesm): Convert this to visitor pattern
static Status TypeToFlatbuffer(FBB& fbb, const std::shared_ptr<DataType>& type,
                               std::vector<FieldOffset>* children,
                               std::vector<VectorLayoutOffset>* layout,
                               flatbuf::Type* out_type, DictionaryMemo* dictionary_memo,
                               Offset* offset) {
  if (type->id() == Type::DICTIONARY) {
    // In this library, the dictionary "type" is a logical construct. Here we
    // pass through to the value type, as we've already captured the index
    // type in the DictionaryEncoding metadata in the parent field
    const auto& dict_type = static_cast<const DictionaryType&>(*type);
    return TypeToFlatbuffer(fbb, dict_type.dictionary()->type(), children, layout,
                            out_type, dictionary_memo, offset);
  }

  std::vector<BufferDescr> buffer_layout = type->GetBufferLayout();
  for (const BufferDescr& descr : buffer_layout) {
    flatbuf::VectorType vector_type;
    switch (descr.type()) {
      case BufferType::OFFSET:
        vector_type = flatbuf::VectorType_OFFSET;
        break;
      case BufferType::DATA:
        vector_type = flatbuf::VectorType_DATA;
        break;
      case BufferType::VALIDITY:
        vector_type = flatbuf::VectorType_VALIDITY;
        break;
      case BufferType::TYPE:
        vector_type = flatbuf::VectorType_TYPE;
        break;
      default:
        vector_type = flatbuf::VectorType_DATA;
        break;
    }
    auto offset = flatbuf::CreateVectorLayout(
        fbb, static_cast<int16_t>(descr.bit_width()), vector_type);
    layout->push_back(offset);
  }

  switch (type->id()) {
    case Type::BOOL:
      *out_type = flatbuf::Type_Bool;
      *offset = flatbuf::CreateBool(fbb).Union();
      break;
    case Type::UINT8:
      INT_TO_FB_CASE(8, false);
    case Type::INT8:
      INT_TO_FB_CASE(8, true);
    case Type::UINT16:
      INT_TO_FB_CASE(16, false);
    case Type::INT16:
      INT_TO_FB_CASE(16, true);
    case Type::UINT32:
      INT_TO_FB_CASE(32, false);
    case Type::INT32:
      INT_TO_FB_CASE(32, true);
    case Type::UINT64:
      INT_TO_FB_CASE(64, false);
    case Type::INT64:
      INT_TO_FB_CASE(64, true);
    case Type::FLOAT:
      *out_type = flatbuf::Type_FloatingPoint;
      *offset = FloatToFlatbuffer(fbb, flatbuf::Precision_SINGLE);
      break;
    case Type::DOUBLE:
      *out_type = flatbuf::Type_FloatingPoint;
      *offset = FloatToFlatbuffer(fbb, flatbuf::Precision_DOUBLE);
      break;
    case Type::FIXED_SIZE_BINARY: {
      const auto& fw_type = static_cast<const FixedSizeBinaryType&>(*type);
      *out_type = flatbuf::Type_FixedSizeBinary;
      *offset = flatbuf::CreateFixedSizeBinary(fbb, fw_type.byte_width()).Union();
    } break;
    case Type::BINARY:
      *out_type = flatbuf::Type_Binary;
      *offset = flatbuf::CreateBinary(fbb).Union();
      break;
    case Type::STRING:
      *out_type = flatbuf::Type_Utf8;
      *offset = flatbuf::CreateUtf8(fbb).Union();
      break;
    case Type::DATE32:
      *out_type = flatbuf::Type_Date;
      *offset = flatbuf::CreateDate(fbb, flatbuf::DateUnit_DAY).Union();
      break;
    case Type::DATE64:
      *out_type = flatbuf::Type_Date;
      *offset = flatbuf::CreateDate(fbb, flatbuf::DateUnit_MILLISECOND).Union();
      break;
    case Type::TIME32: {
      const auto& time_type = static_cast<const Time32Type&>(*type);
      *out_type = flatbuf::Type_Time;
      *offset = flatbuf::CreateTime(fbb, ToFlatbufferUnit(time_type.unit()), 32).Union();
    } break;
    case Type::TIME64: {
      const auto& time_type = static_cast<const Time64Type&>(*type);
      *out_type = flatbuf::Type_Time;
      *offset = flatbuf::CreateTime(fbb, ToFlatbufferUnit(time_type.unit()), 64).Union();
    } break;
    case Type::TIMESTAMP: {
      const auto& ts_type = static_cast<const TimestampType&>(*type);
      *out_type = flatbuf::Type_Timestamp;

      flatbuf::TimeUnit fb_unit = ToFlatbufferUnit(ts_type.unit());
      FBString fb_timezone = 0;
      if (ts_type.timezone().size() > 0) {
        fb_timezone = fbb.CreateString(ts_type.timezone());
      }
      *offset = flatbuf::CreateTimestamp(fbb, fb_unit, fb_timezone).Union();
    } break;
    case Type::LIST:
      *out_type = flatbuf::Type_List;
      return ListToFlatbuffer(fbb, type, children, dictionary_memo, offset);
    case Type::STRUCT:
      *out_type = flatbuf::Type_Struct_;
      return StructToFlatbuffer(fbb, type, children, dictionary_memo, offset);
    case Type::UNION:
      *out_type = flatbuf::Type_Union;
      return UnionToFlatBuffer(fbb, type, children, dictionary_memo, offset);
    default:
      *out_type = flatbuf::Type_NONE;  // Make clang-tidy happy
      std::stringstream ss;
      ss << "Unable to convert type: " << type->ToString() << std::endl;
      return Status::NotImplemented(ss.str());
  }
  return Status::OK();
}

static Status TensorTypeToFlatbuffer(FBB& fbb, const std::shared_ptr<DataType>& type,
                                     flatbuf::Type* out_type, Offset* offset) {
  switch (type->id()) {
    case Type::UINT8:
      INT_TO_FB_CASE(8, false);
    case Type::INT8:
      INT_TO_FB_CASE(8, true);
    case Type::UINT16:
      INT_TO_FB_CASE(16, false);
    case Type::INT16:
      INT_TO_FB_CASE(16, true);
    case Type::UINT32:
      INT_TO_FB_CASE(32, false);
    case Type::INT32:
      INT_TO_FB_CASE(32, true);
    case Type::UINT64:
      INT_TO_FB_CASE(64, false);
    case Type::INT64:
      INT_TO_FB_CASE(64, true);
    case Type::HALF_FLOAT:
      *out_type = flatbuf::Type_FloatingPoint;
      *offset = FloatToFlatbuffer(fbb, flatbuf::Precision_HALF);
      break;
    case Type::FLOAT:
      *out_type = flatbuf::Type_FloatingPoint;
      *offset = FloatToFlatbuffer(fbb, flatbuf::Precision_SINGLE);
      break;
    case Type::DOUBLE:
      *out_type = flatbuf::Type_FloatingPoint;
      *offset = FloatToFlatbuffer(fbb, flatbuf::Precision_DOUBLE);
      break;
    default:
      *out_type = flatbuf::Type_NONE;  // Make clang-tidy happy
      std::stringstream ss;
      ss << "Unable to convert type: " << type->ToString() << std::endl;
      return Status::NotImplemented(ss.str());
  }
  return Status::OK();
}

static DictionaryOffset GetDictionaryEncoding(FBB& fbb, const DictionaryType& type,
                                              DictionaryMemo* memo) {
  int64_t dictionary_id = memo->GetId(type.dictionary());

  // We assume that the dictionary index type (as an integer) has already been
  // validated elsewhere, and can safely assume we are dealing with signed
  // integers
  const auto& fw_index_type = static_cast<const FixedWidthType&>(*type.index_type());

  auto index_type_offset = flatbuf::CreateInt(fbb, fw_index_type.bit_width(), true);

  // TODO(wesm): ordered dictionaries
  return flatbuf::CreateDictionaryEncoding(fbb, dictionary_id, index_type_offset,
                                           type.ordered());
}

static Status FieldToFlatbuffer(FBB& fbb, const std::shared_ptr<Field>& field,
                                DictionaryMemo* dictionary_memo, FieldOffset* offset) {
  auto fb_name = fbb.CreateString(field->name());

  flatbuf::Type type_enum;
  Offset type_offset;
  Offset type_layout;
  std::vector<FieldOffset> children;
  std::vector<VectorLayoutOffset> layout;

  RETURN_NOT_OK(TypeToFlatbuffer(fbb, field->type(), &children, &layout, &type_enum,
                                 dictionary_memo, &type_offset));
  auto fb_children = fbb.CreateVector(children);
  auto fb_layout = fbb.CreateVector(layout);

  DictionaryOffset dictionary = 0;
  if (field->type()->id() == Type::DICTIONARY) {
    dictionary = GetDictionaryEncoding(
        fbb, static_cast<const DictionaryType&>(*field->type()), dictionary_memo);
  }

  // TODO: produce the list of VectorTypes
  *offset = flatbuf::CreateField(fbb, fb_name, field->nullable(), type_enum, type_offset,
                                 dictionary, fb_children, fb_layout);

  return Status::OK();
}

static Status FieldFromFlatbuffer(const flatbuf::Field* field,
                                  const DictionaryMemo& dictionary_memo,
                                  std::shared_ptr<Field>* out) {
  std::shared_ptr<DataType> type;

  const flatbuf::DictionaryEncoding* encoding = field->dictionary();

  if (encoding == nullptr) {
    // The field is not dictionary encoded. We must potentially visit its
    // children to fully reconstruct the data type
    auto children = field->children();
    std::vector<std::shared_ptr<Field>> child_fields(children->size());
    for (int i = 0; i < static_cast<int>(children->size()); ++i) {
      RETURN_NOT_OK(
          FieldFromFlatbuffer(children->Get(i), dictionary_memo, &child_fields[i]));
    }
    RETURN_NOT_OK(
        TypeFromFlatbuffer(field->type_type(), field->type(), child_fields, &type));
  } else {
    // The field is dictionary encoded. The type of the dictionary values has
    // been determined elsewhere, and is stored in the DictionaryMemo. Here we
    // construct the logical DictionaryType object

    std::shared_ptr<Array> dictionary;
    RETURN_NOT_OK(dictionary_memo.GetDictionary(encoding->id(), &dictionary));

    std::shared_ptr<DataType> index_type;
    RETURN_NOT_OK(IntFromFlatbuffer(encoding->indexType(), &index_type));
    type = ::arrow::dictionary(index_type, dictionary, encoding->isOrdered());
  }
  *out = std::make_shared<Field>(field->name()->str(), type, field->nullable());
  return Status::OK();
}

static Status FieldFromFlatbufferDictionary(const flatbuf::Field* field,
                                            std::shared_ptr<Field>* out) {
  // Need an empty memo to pass down for constructing children
  DictionaryMemo dummy_memo;

  // Any DictionaryEncoding set is ignored here

  std::shared_ptr<DataType> type;
  auto children = field->children();
  std::vector<std::shared_ptr<Field>> child_fields(children->size());
  for (int i = 0; i < static_cast<int>(children->size()); ++i) {
    RETURN_NOT_OK(FieldFromFlatbuffer(children->Get(i), dummy_memo, &child_fields[i]));
  }

  RETURN_NOT_OK(
      TypeFromFlatbuffer(field->type_type(), field->type(), child_fields, &type));

  *out = std::make_shared<Field>(field->name()->str(), type, field->nullable());
  return Status::OK();
}

// will return the endianness of the system we are running on
// based the NUMPY_API function. See NOTICE.txt
flatbuf::Endianness endianness() {
  union {
    uint32_t i;
    char c[4];
  } bint = {0x01020304};

  return bint.c[0] == 1 ? flatbuf::Endianness_Big : flatbuf::Endianness_Little;
}

static Status SchemaToFlatbuffer(FBB& fbb, const Schema& schema,
                                 DictionaryMemo* dictionary_memo,
                                 flatbuffers::Offset<flatbuf::Schema>* out) {
  /// Fields
  std::vector<FieldOffset> field_offsets;
  for (int i = 0; i < schema.num_fields(); ++i) {
    std::shared_ptr<Field> field = schema.field(i);
    FieldOffset offset;
    RETURN_NOT_OK(FieldToFlatbuffer(fbb, field, dictionary_memo, &offset));
    field_offsets.push_back(offset);
  }

  auto fb_offsets = fbb.CreateVector(field_offsets);

  /// Custom metadata
  const KeyValueMetadata* metadata = schema.metadata().get();

  if (metadata != nullptr) {
    std::vector<KeyValueOffset> key_value_offsets;
    size_t metadata_size = metadata->size();
    key_value_offsets.reserve(metadata_size);
    for (size_t i = 0; i < metadata_size; ++i) {
      const auto& key = metadata->key(i);
      const auto& value = metadata->value(i);
      key_value_offsets.push_back(
          flatbuf::CreateKeyValue(fbb, fbb.CreateString(key), fbb.CreateString(value)));
    }
    *out = flatbuf::CreateSchema(fbb, endianness(), fb_offsets,
                                 fbb.CreateVector(key_value_offsets));
  } else {
    *out = flatbuf::CreateSchema(fbb, endianness(), fb_offsets);
  }

  return Status::OK();
}

static Status WriteFlatbufferBuilder(FBB& fbb, std::shared_ptr<Buffer>* out) {
  int32_t size = fbb.GetSize();

  auto result = std::make_shared<PoolBuffer>();
  RETURN_NOT_OK(result->Resize(size));

  uint8_t* dst = result->mutable_data();
  memcpy(dst, fbb.GetBufferPointer(), size);
  *out = result;
  return Status::OK();
}

static Status WriteFBMessage(FBB& fbb, flatbuf::MessageHeader header_type,
                             flatbuffers::Offset<void> header, int64_t body_length,
                             std::shared_ptr<Buffer>* out) {
  auto message = flatbuf::CreateMessage(fbb, kCurrentMetadataVersion, header_type, header,
                                        body_length);
  fbb.Finish(message);
  return WriteFlatbufferBuilder(fbb, out);
}

Status WriteSchemaMessage(const Schema& schema, DictionaryMemo* dictionary_memo,
                          std::shared_ptr<Buffer>* out) {
  FBB fbb;
  flatbuffers::Offset<flatbuf::Schema> fb_schema;
  RETURN_NOT_OK(SchemaToFlatbuffer(fbb, schema, dictionary_memo, &fb_schema));
  return WriteFBMessage(fbb, flatbuf::MessageHeader_Schema, fb_schema.Union(), 0, out);
}

using FieldNodeVector =
    flatbuffers::Offset<flatbuffers::Vector<const flatbuf::FieldNode*>>;
using BufferVector = flatbuffers::Offset<flatbuffers::Vector<const flatbuf::Buffer*>>;

static Status WriteFieldNodes(FBB& fbb, const std::vector<FieldMetadata>& nodes,
                              FieldNodeVector* out) {
  std::vector<flatbuf::FieldNode> fb_nodes;
  fb_nodes.reserve(nodes.size());

  for (size_t i = 0; i < nodes.size(); ++i) {
    const FieldMetadata& node = nodes[i];
    if (node.offset != 0) {
      return Status::Invalid("Field metadata for IPC must have offset 0");
    }
    fb_nodes.emplace_back(node.length, node.null_count);
  }
  *out = fbb.CreateVectorOfStructs(fb_nodes);
  return Status::OK();
}

static Status WriteBuffers(FBB& fbb, const std::vector<BufferMetadata>& buffers,
                           BufferVector* out) {
  std::vector<flatbuf::Buffer> fb_buffers;
  fb_buffers.reserve(buffers.size());

  for (size_t i = 0; i < buffers.size(); ++i) {
    const BufferMetadata& buffer = buffers[i];
    fb_buffers.emplace_back(buffer.page, buffer.offset, buffer.length);
  }
  *out = fbb.CreateVectorOfStructs(fb_buffers);
  return Status::OK();
}

static Status MakeRecordBatch(FBB& fbb, int64_t length, int64_t body_length,
                              const std::vector<FieldMetadata>& nodes,
                              const std::vector<BufferMetadata>& buffers,
                              RecordBatchOffset* offset) {
  FieldNodeVector fb_nodes;
  BufferVector fb_buffers;

  RETURN_NOT_OK(WriteFieldNodes(fbb, nodes, &fb_nodes));
  RETURN_NOT_OK(WriteBuffers(fbb, buffers, &fb_buffers));

  *offset = flatbuf::CreateRecordBatch(fbb, length, fb_nodes, fb_buffers);
  return Status::OK();
}

Status WriteRecordBatchMessage(int64_t length, int64_t body_length,
                               const std::vector<FieldMetadata>& nodes,
                               const std::vector<BufferMetadata>& buffers,
                               std::shared_ptr<Buffer>* out) {
  FBB fbb;
  RecordBatchOffset record_batch;
  RETURN_NOT_OK(MakeRecordBatch(fbb, length, body_length, nodes, buffers, &record_batch));
  return WriteFBMessage(fbb, flatbuf::MessageHeader_RecordBatch, record_batch.Union(),
                        body_length, out);
}

Status WriteTensorMessage(const Tensor& tensor, int64_t buffer_start_offset,
                          std::shared_ptr<Buffer>* out) {
  using TensorDimOffset = flatbuffers::Offset<flatbuf::TensorDim>;
  using TensorOffset = flatbuffers::Offset<flatbuf::Tensor>;

  FBB fbb;

  flatbuf::Type fb_type_type;
  Offset fb_type;
  RETURN_NOT_OK(TensorTypeToFlatbuffer(fbb, tensor.type(), &fb_type_type, &fb_type));

  std::vector<TensorDimOffset> dims;
  for (int i = 0; i < tensor.ndim(); ++i) {
    FBString name = fbb.CreateString(tensor.dim_name(i));
    dims.push_back(flatbuf::CreateTensorDim(fbb, tensor.shape()[i], name));
  }

  auto fb_shape = fbb.CreateVector(dims);
  auto fb_strides = fbb.CreateVector(tensor.strides());
  int64_t body_length = tensor.data()->size();
  flatbuf::Buffer buffer(-1, buffer_start_offset, body_length);

  TensorOffset fb_tensor =
      flatbuf::CreateTensor(fbb, fb_type_type, fb_type, fb_shape, fb_strides, &buffer);

  return WriteFBMessage(fbb, flatbuf::MessageHeader_Tensor, fb_tensor.Union(),
                        body_length, out);
}

Status WriteDictionaryMessage(int64_t id, int64_t length, int64_t body_length,
                              const std::vector<FieldMetadata>& nodes,
                              const std::vector<BufferMetadata>& buffers,
                              std::shared_ptr<Buffer>* out) {
  FBB fbb;
  RecordBatchOffset record_batch;
  RETURN_NOT_OK(MakeRecordBatch(fbb, length, body_length, nodes, buffers, &record_batch));
  auto dictionary_batch = flatbuf::CreateDictionaryBatch(fbb, id, record_batch).Union();
  return WriteFBMessage(fbb, flatbuf::MessageHeader_DictionaryBatch, dictionary_batch,
                        body_length, out);
}

static flatbuffers::Offset<flatbuffers::Vector<const flatbuf::Block*>>
FileBlocksToFlatbuffer(FBB& fbb, const std::vector<FileBlock>& blocks) {
  std::vector<flatbuf::Block> fb_blocks;

  for (const FileBlock& block : blocks) {
    fb_blocks.emplace_back(block.offset, block.metadata_length, block.body_length);
  }

  return fbb.CreateVectorOfStructs(fb_blocks);
}

Status WriteFileFooter(const Schema& schema, const std::vector<FileBlock>& dictionaries,
                       const std::vector<FileBlock>& record_batches,
                       DictionaryMemo* dictionary_memo, io::OutputStream* out) {
  FBB fbb;

  flatbuffers::Offset<flatbuf::Schema> fb_schema;
  RETURN_NOT_OK(SchemaToFlatbuffer(fbb, schema, dictionary_memo, &fb_schema));

  auto fb_dictionaries = FileBlocksToFlatbuffer(fbb, dictionaries);
  auto fb_record_batches = FileBlocksToFlatbuffer(fbb, record_batches);

  auto footer = flatbuf::CreateFooter(fbb, kCurrentMetadataVersion, fb_schema,
                                      fb_dictionaries, fb_record_batches);

  fbb.Finish(footer);

  int32_t size = fbb.GetSize();

  return out->Write(fbb.GetBufferPointer(), size);
}

// ----------------------------------------------------------------------
// Memoization data structure for handling shared dictionaries

DictionaryMemo::DictionaryMemo() {}

// Returns KeyError if dictionary not found
Status DictionaryMemo::GetDictionary(int64_t id,
                                     std::shared_ptr<Array>* dictionary) const {
  auto it = id_to_dictionary_.find(id);
  if (it == id_to_dictionary_.end()) {
    std::stringstream ss;
    ss << "Dictionary with id " << id << " not found";
    return Status::KeyError(ss.str());
  }
  *dictionary = it->second;
  return Status::OK();
}

int64_t DictionaryMemo::GetId(const std::shared_ptr<Array>& dictionary) {
  intptr_t address = reinterpret_cast<intptr_t>(dictionary.get());
  auto it = dictionary_to_id_.find(address);
  if (it != dictionary_to_id_.end()) {
    // Dictionary already observed, return the id
    return it->second;
  } else {
    int64_t new_id = static_cast<int64_t>(dictionary_to_id_.size());
    dictionary_to_id_[address] = new_id;
    id_to_dictionary_[new_id] = dictionary;
    return new_id;
  }
}

bool DictionaryMemo::HasDictionary(const std::shared_ptr<Array>& dictionary) const {
  intptr_t address = reinterpret_cast<intptr_t>(dictionary.get());
  auto it = dictionary_to_id_.find(address);
  return it != dictionary_to_id_.end();
}

bool DictionaryMemo::HasDictionaryId(int64_t id) const {
  auto it = id_to_dictionary_.find(id);
  return it != id_to_dictionary_.end();
}

Status DictionaryMemo::AddDictionary(int64_t id,
                                     const std::shared_ptr<Array>& dictionary) {
  if (HasDictionaryId(id)) {
    std::stringstream ss;
    ss << "Dictionary with id " << id << " already exists";
    return Status::KeyError(ss.str());
  }
  intptr_t address = reinterpret_cast<intptr_t>(dictionary.get());
  id_to_dictionary_[id] = dictionary;
  dictionary_to_id_[address] = id;
  return Status::OK();
}

//----------------------------------------------------------------------
// Message reader

class Message::MessageImpl {
 public:
  explicit MessageImpl(const std::shared_ptr<Buffer>& metadata,
                       const std::shared_ptr<Buffer>& body)
      : metadata_(metadata), message_(nullptr), body_(body) {}

  Status Open() {
    message_ = flatbuf::GetMessage(metadata_->data());

    // Check that the metadata version is supported
    if (message_->version() < kMinMetadataVersion) {
      return Status::Invalid("Old metadata version not supported");
    }

    return Status::OK();
  }

  Message::Type type() const {
    switch (message_->header_type()) {
      case flatbuf::MessageHeader_Schema:
        return Message::SCHEMA;
      case flatbuf::MessageHeader_DictionaryBatch:
        return Message::DICTIONARY_BATCH;
      case flatbuf::MessageHeader_RecordBatch:
        return Message::RECORD_BATCH;
      case flatbuf::MessageHeader_Tensor:
        return Message::TENSOR;
      default:
        return Message::NONE;
    }
  }

  MetadataVersion version() const {
    switch (message_->version()) {
      case flatbuf::MetadataVersion_V1:
        // Arrow 0.1
        return MetadataVersion::V1;
      case flatbuf::MetadataVersion_V2:
        // Arrow 0.2
        return MetadataVersion::V2;
      case flatbuf::MetadataVersion_V3:
        // Arrow >= 0.3
        return MetadataVersion::V3;
      // Add cases as other versions become available
      default:
        return MetadataVersion::V3;
    }
  }

  const void* header() const { return message_->header(); }

  std::shared_ptr<Buffer> body() const { return body_; }

  std::shared_ptr<Buffer> metadata() const { return metadata_; }

 private:
  // The Flatbuffer metadata
  std::shared_ptr<Buffer> metadata_;
  const flatbuf::Message* message_;

  // The message body, if any
  std::shared_ptr<Buffer> body_;
};

Message::Message(const std::shared_ptr<Buffer>& metadata,
                 const std::shared_ptr<Buffer>& body) {
  impl_.reset(new MessageImpl(metadata, body));
}

Status Message::Open(const std::shared_ptr<Buffer>& metadata,
                     const std::shared_ptr<Buffer>& body, std::unique_ptr<Message>* out) {
  out->reset(new Message(metadata, body));
  return (*out)->impl_->Open();
}

Message::~Message() {}

std::shared_ptr<Buffer> Message::body() const { return impl_->body(); }

std::shared_ptr<Buffer> Message::metadata() const { return impl_->metadata(); }

Message::Type Message::type() const { return impl_->type(); }

MetadataVersion Message::metadata_version() const { return impl_->version(); }

const void* Message::header() const { return impl_->header(); }

bool Message::Equals(const Message& other) const {
  int64_t metadata_bytes = std::min(metadata()->size(), other.metadata()->size());

  if (!metadata()->Equals(*other.metadata(), metadata_bytes)) {
    return false;
  }

  // Compare bodies, if they have them
  auto this_body = body();
  auto other_body = other.body();

  const bool this_has_body = (this_body != nullptr) && (this_body->size() > 0);
  const bool other_has_body = (other_body != nullptr) && (other_body->size() > 0);

  if (this_has_body && other_has_body) {
    return this_body->Equals(*other_body);
  } else if (this_has_body ^ other_has_body) {
    // One has a body but not the other
    return false;
  } else {
    // Neither has a body
    return true;
  }
}

Status Message::SerializeTo(io::OutputStream* file, int64_t* output_length) const {
  int32_t metadata_length = 0;
  RETURN_NOT_OK(WriteMessage(*metadata(), file, &metadata_length));

  *output_length = metadata_length;

  auto body_buffer = body();
  if (body_buffer) {
    RETURN_NOT_OK(file->Write(body_buffer->data(), body_buffer->size()));
    *output_length += body_buffer->size();
  }

  return Status::OK();
}

std::string FormatMessageType(Message::Type type) {
  switch (type) {
    case Message::SCHEMA:
      return "schema";
    case Message::RECORD_BATCH:
      return "record batch";
    case Message::DICTIONARY_BATCH:
      return "dictionary";
    default:
      break;
  }
  return "unknown";
}

// ----------------------------------------------------------------------

static Status VisitField(const flatbuf::Field* field, DictionaryTypeMap* id_to_field) {
  const flatbuf::DictionaryEncoding* dict_metadata = field->dictionary();
  if (dict_metadata == nullptr) {
    // Field is not dictionary encoded. Visit children
    auto children = field->children();
    for (flatbuffers::uoffset_t i = 0; i < children->size(); ++i) {
      RETURN_NOT_OK(VisitField(children->Get(i), id_to_field));
    }
  } else {
    // Field is dictionary encoded. Construct the data type for the
    // dictionary (no descendents can be dictionary encoded)
    std::shared_ptr<Field> dictionary_field;
    RETURN_NOT_OK(FieldFromFlatbufferDictionary(field, &dictionary_field));
    (*id_to_field)[dict_metadata->id()] = dictionary_field;
  }
  return Status::OK();
}

Status GetDictionaryTypes(const void* opaque_schema, DictionaryTypeMap* id_to_field) {
  auto schema = static_cast<const flatbuf::Schema*>(opaque_schema);
  int num_fields = static_cast<int>(schema->fields()->size());
  for (int i = 0; i < num_fields; ++i) {
    RETURN_NOT_OK(VisitField(schema->fields()->Get(i), id_to_field));
  }
  return Status::OK();
}

Status GetSchema(const void* opaque_schema, const DictionaryMemo& dictionary_memo,
                 std::shared_ptr<Schema>* out) {
  auto schema = static_cast<const flatbuf::Schema*>(opaque_schema);
  int num_fields = static_cast<int>(schema->fields()->size());

  std::vector<std::shared_ptr<Field>> fields(num_fields);
  for (int i = 0; i < num_fields; ++i) {
    const flatbuf::Field* field = schema->fields()->Get(i);
    RETURN_NOT_OK(FieldFromFlatbuffer(field, dictionary_memo, &fields[i]));
  }

  auto metadata = std::make_shared<KeyValueMetadata>();
  auto fb_metadata = schema->custom_metadata();
  if (fb_metadata != nullptr) {
    metadata->reserve(fb_metadata->size());
    for (const auto& pair : *fb_metadata) {
      metadata->Append(pair->key()->str(), pair->value()->str());
    }
  }

  *out = std::make_shared<Schema>(fields, metadata);
  return Status::OK();
}

Status GetTensorMetadata(const Buffer& metadata, std::shared_ptr<DataType>* type,
                         std::vector<int64_t>* shape, std::vector<int64_t>* strides,
                         std::vector<std::string>* dim_names) {
  auto message = flatbuf::GetMessage(metadata.data());
  auto tensor = reinterpret_cast<const flatbuf::Tensor*>(message->header());

  int ndim = static_cast<int>(tensor->shape()->size());

  for (int i = 0; i < ndim; ++i) {
    auto dim = tensor->shape()->Get(i);

    shape->push_back(dim->size());
    auto fb_name = dim->name();
    if (fb_name == 0) {
      dim_names->push_back("");
    } else {
      dim_names->push_back(fb_name->str());
    }
  }

  if (tensor->strides()->size() > 0) {
    for (int i = 0; i < ndim; ++i) {
      strides->push_back(tensor->strides()->Get(i));
    }
  }

  return TypeFromFlatbuffer(tensor->type_type(), tensor->type(), {}, type);
}

// ----------------------------------------------------------------------
// Read and write messages

static Status ReadFullMessage(const std::shared_ptr<Buffer>& metadata,
                              io::InputStream* stream,
                              std::unique_ptr<Message>* message) {
  auto fb_message = flatbuf::GetMessage(metadata->data());

  int64_t body_length = fb_message->bodyLength();

  std::shared_ptr<Buffer> body;
  RETURN_NOT_OK(stream->Read(body_length, &body));

  if (body->size() < body_length) {
    std::stringstream ss;
    ss << "Expected to be able to read " << body_length << " bytes for message body, got "
       << body->size();
    return Status::IOError(ss.str());
  }

  return Message::Open(metadata, body, message);
}

Status ReadMessage(int64_t offset, int32_t metadata_length, io::RandomAccessFile* file,
                   std::unique_ptr<Message>* message) {
  std::shared_ptr<Buffer> buffer;
  RETURN_NOT_OK(file->ReadAt(offset, metadata_length, &buffer));

  int32_t flatbuffer_size = *reinterpret_cast<const int32_t*>(buffer->data());

  if (flatbuffer_size + static_cast<int>(sizeof(int32_t)) > metadata_length) {
    std::stringstream ss;
    ss << "flatbuffer size " << metadata_length << " invalid. File offset: " << offset
       << ", metadata length: " << metadata_length;
    return Status::Invalid(ss.str());
  }

  auto metadata = SliceBuffer(buffer, 4, buffer->size() - 4);
  return ReadFullMessage(metadata, file, message);
}

Status ReadMessage(io::InputStream* file, std::unique_ptr<Message>* message) {
  std::shared_ptr<Buffer> buffer;

  RETURN_NOT_OK(file->Read(sizeof(int32_t), &buffer));
  if (buffer->size() != sizeof(int32_t)) {
    *message = nullptr;
    return Status::OK();
  }

  int32_t message_length = *reinterpret_cast<const int32_t*>(buffer->data());

  if (message_length == 0) {
    // Optional 0 EOS control message
    *message = nullptr;
    return Status::OK();
  }

  RETURN_NOT_OK(file->Read(message_length, &buffer));
  if (buffer->size() != message_length) {
    return Status::IOError("Unexpected end of stream trying to read message");
  }

  return ReadFullMessage(buffer, file, message);
}

// ----------------------------------------------------------------------
// Implement InputStream message reader

Status InputStreamMessageReader::ReadNextMessage(std::unique_ptr<Message>* message) {
  return ReadMessage(stream_.get(), message);
}

InputStreamMessageReader::~InputStreamMessageReader() {}

// ----------------------------------------------------------------------
// Implement message writing

Status WriteMessage(const Buffer& message, io::OutputStream* file,
                    int32_t* message_length) {
  // Need to write 4 bytes (message size), the message, plus padding to
  // end on an 8-byte offset
  int64_t start_offset;
  RETURN_NOT_OK(file->Tell(&start_offset));

  int32_t padded_message_length = static_cast<int32_t>(message.size()) + 4;
  const int32_t remainder =
      (padded_message_length + static_cast<int32_t>(start_offset)) % 8;
  if (remainder != 0) {
    padded_message_length += 8 - remainder;
  }

  // The returned message size includes the length prefix, the flatbuffer,
  // plus padding
  *message_length = padded_message_length;

  // Write the flatbuffer size prefix including padding
  int32_t flatbuffer_size = padded_message_length - 4;
  RETURN_NOT_OK(
      file->Write(reinterpret_cast<const uint8_t*>(&flatbuffer_size), sizeof(int32_t)));

  // Write the flatbuffer
  RETURN_NOT_OK(file->Write(message.data(), message.size()));

  // Write any padding
  int32_t padding = padded_message_length - static_cast<int32_t>(message.size()) - 4;
  if (padding > 0) {
    RETURN_NOT_OK(file->Write(kPaddingBytes, padding));
  }

  return Status::OK();
}

}  // namespace ipc
}  // namespace arrow
