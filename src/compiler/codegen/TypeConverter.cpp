// SukiCode type to LLVM type converter implementation.

#include "TypeConverter.h"

#ifdef SUKI_HAS_LLVM
#include <llvm/IR/DerivedTypes.h>
#endif

namespace suki {

#ifdef SUKI_HAS_LLVM

llvm::Type* TypeConverter::convert(const Type& type) {
    // Check cache
    auto it = cache_.find(&type);
    if (it != cache_.end()) return it->second;

    llvm::Type* result = nullptr;

    switch (type.kind()) {
        case TypeKind::Void:
            result = getVoidType();
            break;
        case TypeKind::Bool:
            result = getBoolType();
            break;
        case TypeKind::Int:
        case TypeKind::Int8:
        case TypeKind::Int16:
        case TypeKind::Int32:
        case TypeKind::Int64:
            result = getIntType(static_cast<const IntType&>(type).bitWidth());
            break;
        case TypeKind::UInt:
        case TypeKind::UInt8:
        case TypeKind::UInt16:
        case TypeKind::UInt32:
        case TypeKind::UInt64:
            result = getIntType(static_cast<const UIntType&>(type).bitWidth());
            break;
        case TypeKind::Float:
            if (static_cast<const FloatType&>(type).bitWidth() == 32) {
                result = getFloatType();
            } else {
                result = getDoubleType();
            }
            break;
        case TypeKind::Double:
            result = getDoubleType();
            break;
        case TypeKind::Char:
            result = getIntType(32); // Unicode scalar
            break;
        case TypeKind::String:
            result = getStringType();
            break;
        case TypeKind::Array: {
            // Array is a struct { pointer, length, capacity }
            auto& arr = static_cast<const ArrayType&>(type);
            llvm::Type* elemTy = convert(*arr.elementType());
            auto* ptrTy = llvm::PointerType::get(ctx_, 0);
            result = llvm::StructType::get(ctx_, {ptrTy, getIntType(64), getIntType(64)});
            break;
        }
        case TypeKind::Dictionary: {
            // Dict is an opaque pointer (runtime managed)
            result = getPtrType();
            break;
        }
        case TypeKind::Optional: {
            // Optional is a struct { value, is_nil_flag }
            auto& opt = static_cast<const OptionalType&>(type);
            llvm::Type* baseTy = convert(*opt.baseType());
            result = llvm::StructType::get(ctx_, {baseTy, getBoolType()});
            break;
        }
        case TypeKind::Tuple: {
            auto& tuple = static_cast<const TupleType&>(type);
            std::vector<llvm::Type*> elemTys;
            for (const auto& elem : tuple.elements()) {
                elemTys.push_back(convert(*elem.type));
            }
            result = llvm::StructType::get(ctx_, elemTys);
            break;
        }
        case TypeKind::Function: {
            // Function type is a pointer to function
            auto& func = static_cast<const FunctionType&>(type);
            llvm::Type* retTy = convert(*func.returnType());
            std::vector<llvm::Type*> paramTys;
            for (const auto& p : func.params()) {
                paramTys.push_back(convert(*p.type));
            }
            auto* fnTy = llvm::FunctionType::get(retTy, paramTys, false);
            result = llvm::PointerType::get(fnTy, 0);
            break;
        }
        case TypeKind::Struct: {
            // Struct is a named struct type
            auto& str = static_cast<const StructType&>(type);
            std::vector<llvm::Type*> fieldTys;
            for (const auto& f : str.fields()) {
                fieldTys.push_back(convert(*f.type));
            }
            result = llvm::StructType::get(ctx_, fieldTys);
            break;
        }
        case TypeKind::Class:
        case TypeKind::Actor:
            // Reference types are pointers
            result = getPtrType();
            break;
        case TypeKind::Enum: {
            // Enum is a struct { tag, payload }
            auto& en = static_cast<const EnumType&>(type);
            llvm::Type* tagTy = getIntType(32);
            // Find max payload size
            uint64_t maxPayloadSize = 0;
            for (const auto& c : en.cases()) {
                uint64_t payloadSize = 0;
                for (const auto& t : c.associatedTypes) {
                    payloadSize += convert(*t)->getPrimitiveSizeInBits() / 8;
                }
                maxPayloadSize = std::max(maxPayloadSize, payloadSize);
            }
            llvm::Type* payloadTy = maxPayloadSize > 0 ?
                llvm::ArrayType::get(getIntType(8), maxPayloadSize) : getIntType(8);
            result = llvm::StructType::get(ctx_, {tagTy, payloadTy});
            break;
        }
        case TypeKind::Any:
        case TypeKind::AnyObject:
            // Any/AnyObject is an opaque pointer
            result = getPtrType();
            break;
        case TypeKind::Owned:
            // Owned<T> has the same layout as T
            result = convert(*static_cast<const OwnedType&>(type).innerType());
            break;
        default:
            result = getVoidType(); // fallback
            break;
    }

    cache_[&type] = result;
    return result;
}

llvm::StructType* TypeConverter::getStringType() {
    // String is { i8*, i64 } (pointer + length)
    static llvm::StructType* stringType = nullptr;
    if (!stringType) {
        stringType = llvm::StructType::get(ctx_, {
            llvm::PointerType::get(ctx_, 0), // data pointer
            getIntType(64)                    // length
        });
    }
    return stringType;
}

llvm::ArrayType* TypeConverter::getArrayType(llvm::Type* elemType, uint64_t count) {
    return llvm::ArrayType::get(elemType, count);
}

#endif

} // namespace suki
