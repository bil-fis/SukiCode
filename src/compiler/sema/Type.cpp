// SukiCode type system implementation.

#include "Type.h"
#include <sstream>
#include <algorithm>

namespace suki {

bool Type::isPrimitive() const {
    return kind_ >= TypeKind::Void && kind_ <= TypeKind::String;
}

bool Type::isReferenceType() const {
    return kind_ == TypeKind::Class || kind_ == TypeKind::Actor ||
           kind_ == TypeKind::String || kind_ == TypeKind::AnyObject;
}

bool Type::canImplicitlyConvertTo(const Type& target) const {
    if (equals(target)) return true;
    // Any 类型接受所有类型 / Any type accepts all types
    if (target.kind_ == TypeKind::Any) return true;
    // 任何类型可以转换为 Optional / Any type can convert to Optional
    if (target.kind_ == TypeKind::Optional) {
        const auto& opt = static_cast<const OptionalType&>(target);
        return canImplicitlyConvertTo(*opt.baseType());
    }
    // 数值类型隐式转换 / Numeric type implicit conversion
    if (kind_ == TypeKind::Int && target.kind_ == TypeKind::Double) return true;
    if (kind_ == TypeKind::Int && target.kind_ == TypeKind::Float) return true;
    if (kind_ == TypeKind::Float && target.kind_ == TypeKind::Double) return true;
    // 整数字面量可以到任何整数类型 / Integer literals can go to any integer type
    if (kind_ == TypeKind::Int && target.kind_ == TypeKind::Int) return true;
    return false;
}

// TupleType
std::string TupleType::name() const {
    std::ostringstream oss;
    oss << "(";
    for (size_t i = 0; i < elems_.size(); i++) {
        if (i > 0) oss << ", ";
        if (!elems_[i].label.empty()) oss << elems_[i].label << ": ";
        oss << elems_[i].type->name();
    }
    oss << ")";
    return oss.str();
}

size_t TupleType::sizeInBytes() const {
    size_t total = 0;
    for (const auto& e : elems_) total += e.type->sizeInBytes();
    return total;
}

size_t TupleType::alignment() const {
    size_t maxA = 1;
    for (const auto& e : elems_) maxA = std::max(maxA, e.type->alignment());
    return maxA;
}

// FunctionType
std::string FunctionType::name() const {
    std::ostringstream oss;
    oss << "(";
    for (size_t i = 0; i < params_.size(); i++) {
        if (i > 0) oss << ", ";
        if (!params_[i].label.empty()) oss << params_[i].label << ": ";
        oss << params_[i].type->name();
    }
    oss << ") -> ";
    if (async_) oss << "async ";
    if (throws_) oss << "throws ";
    oss << ret_->name();
    return oss.str();
}

// StructType
size_t StructType::sizeInBytes() const {
    size_t total = 0;
    for (const auto& f : fields_) total += f.type->sizeInBytes();
    return total;
}

size_t StructType::alignment() const {
    size_t maxA = 1;
    for (const auto& f : fields_) maxA = std::max(maxA, f.type->alignment());
    return maxA;
}

void StructType::addField(const std::string& n, TypePtr t) {
    Field f;
    f.name = n;
    f.type = std::move(t);
    f.offset = sizeInBytes();
    fields_.push_back(std::move(f));
}

// EnumType
size_t EnumType::sizeInBytes() const {
    size_t maxSize = 4;
    for (const auto& c : cases_) {
        size_t cs = 0;
        for (const auto& t : c.associatedTypes) cs += t->sizeInBytes();
        maxSize = std::max(maxSize, cs);
    }
    return maxSize + 4;
}

size_t EnumType::alignment() const { return 4; }

void EnumType::addCase(const std::string& n, std::vector<TypePtr> assoc) {
    Case c;
    c.name = n;
    c.associatedTypes = std::move(assoc);
    cases_.push_back(std::move(c));
}

// Type factories
TypePtr getVoidType() { static auto i = std::make_shared<VoidType>(); return i; }
TypePtr getBoolType() { static auto i = std::make_shared<BoolType>(); return i; }

TypePtr getIntType(int bits) {
    static std::unordered_map<int, TypePtr> cache;
    auto it = cache.find(bits);
    if (it != cache.end()) return it->second;
    auto t = std::make_shared<IntType>(bits);
    cache[bits] = t;
    return t;
}

TypePtr getUIntType(int bits) {
    static std::unordered_map<int, TypePtr> cache;
    auto it = cache.find(bits);
    if (it != cache.end()) return it->second;
    auto t = std::make_shared<UIntType>(bits);
    cache[bits] = t;
    return t;
}

TypePtr getFloatType(int bits) {
    static std::unordered_map<int, TypePtr> cache;
    auto it = cache.find(bits);
    if (it != cache.end()) return it->second;
    auto t = std::make_shared<FloatType>(bits);
    cache[bits] = t;
    return t;
}

TypePtr getDoubleType() { static auto i = std::make_shared<FloatType>(64); return i; }
TypePtr getCharType() { static auto i = std::make_shared<CharType>(); return i; }
TypePtr getStringType() { static auto i = std::make_shared<StringType>(); return i; }
TypePtr getAnyType() { static auto i = std::make_shared<AnyType>(); return i; }
TypePtr getAnyObjectType() { static auto i = std::make_shared<AnyObjectType>(); return i; }
TypePtr getErrorType() { static auto i = std::make_shared<ErrorType>(); return i; }

TypePtr resolvePrimitiveType(const std::string& name) {
    if (name == "Void") return getVoidType();
    if (name == "Bool") return getBoolType();
    if (name == "Int") return getIntType(64);
    if (name == "Int8") return getIntType(8);
    if (name == "Int16") return getIntType(16);
    if (name == "Int32") return getIntType(32);
    if (name == "Int64") return getIntType(64);
    if (name == "UInt") return getUIntType(64);
    if (name == "UInt8") return getUIntType(8);
    if (name == "UInt16") return getUIntType(16);
    if (name == "UInt32") return getUIntType(32);
    if (name == "UInt64") return getUIntType(64);
    if (name == "Float") return getFloatType(32);
    if (name == "Double") return getDoubleType();
    if (name == "Char") return getCharType();
    if (name == "String") return getStringType();
    return nullptr;
}

} // namespace suki
