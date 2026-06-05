#pragma once
// SukiCode 类型系统定义
// Type system definitions for SukiCode semantic analysis.

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <cstddef>

namespace suki {

class Type;
using TypePtr = std::shared_ptr<Type>;

enum class TypeKind : uint8_t {
    Void, Bool, Int, Int8, Int16, Int32, Int64,
    UInt, UInt8, UInt16, UInt32, UInt64,
    Float, Double, Char, String,
    Array, Dictionary, Set, Optional, Tuple, Function,
    Struct, Class, Enum, Protocol, Actor,
    Any, AnyObject, Self, Owned, Unresolved, Error,
};

class Type {
public:
    explicit Type(TypeKind kind) : kind_(kind) {}
    virtual ~Type() = default;

    TypeKind kind() const { return kind_; }
    virtual bool isPrimitive() const;
    virtual bool isReferenceType() const;
    bool isValueType() const { return !isReferenceType(); }
    virtual std::string name() const = 0;
    virtual bool equals(const Type& other) const { return kind_ == other.kind_; }
    virtual bool canImplicitlyConvertTo(const Type& target) const;
    virtual size_t sizeInBytes() const = 0;
    virtual size_t alignment() const = 0;

private:
    TypeKind kind_;
};

// 基本类型 / Primitive types
class VoidType : public Type {
public:
    VoidType() : Type(TypeKind::Void) {}
    std::string name() const override { return "Void"; }
    size_t sizeInBytes() const override { return 0; }
    size_t alignment() const override { return 0; }
};

class BoolType : public Type {
public:
    BoolType() : Type(TypeKind::Bool) {}
    std::string name() const override { return "Bool"; }
    size_t sizeInBytes() const override { return 1; }
    size_t alignment() const override { return 1; }
};

class IntType : public Type {
public:
    explicit IntType(int bits = 64) : Type(TypeKind::Int), bits_(bits) {}
    std::string name() const override { return "Int"; }
    int bitWidth() const { return bits_; }
    size_t sizeInBytes() const override { return static_cast<size_t>(bits_ / 8); }
    size_t alignment() const override { return static_cast<size_t>(bits_ / 8); }
private:
    int bits_;
};

class FloatType : public Type {
public:
    explicit FloatType(int bits = 32) : Type(TypeKind::Float), bits_(bits) {}
    std::string name() const override { return bits_ == 32 ? "Float" : "Double"; }
    int bitWidth() const { return bits_; }
    size_t sizeInBytes() const override { return static_cast<size_t>(bits_ / 8); }
    size_t alignment() const override { return static_cast<size_t>(bits_ / 8); }
private:
    int bits_;
};

class StringType : public Type {
public:
    StringType() : Type(TypeKind::String) {}
    std::string name() const override { return "String"; }
    bool isReferenceType() const override { return true; }
    size_t sizeInBytes() const override { return sizeof(void*) * 2; }
    size_t alignment() const override { return sizeof(void*); }
};

// 复合类型 / Composite types
class ArrayType : public Type {
public:
    explicit ArrayType(TypePtr elem) : Type(TypeKind::Array), elem_(std::move(elem)) {}
    std::string name() const override { return "[" + elem_->name() + "]"; }
    TypePtr elementType() const { return elem_; }
    size_t sizeInBytes() const override { return sizeof(void*) * 2; }
    size_t alignment() const override { return sizeof(void*); }
private:
    TypePtr elem_;
};

class DictType : public Type {
public:
    DictType(TypePtr key, TypePtr val)
        : Type(TypeKind::Dictionary), key_(std::move(key)), val_(std::move(val)) {}
    std::string name() const override { return "[" + key_->name() + ": " + val_->name() + "]"; }
    TypePtr keyType() const { return key_; }
    TypePtr valueType() const { return val_; }
    size_t sizeInBytes() const override { return sizeof(void*) * 2; }
    size_t alignment() const override { return sizeof(void*); }
private:
    TypePtr key_;
    TypePtr val_;
};

class OptionalType : public Type {
public:
    explicit OptionalType(TypePtr base) : Type(TypeKind::Optional), base_(std::move(base)) {}
    std::string name() const override { return base_->name() + "?"; }
    TypePtr baseType() const { return base_; }
    size_t sizeInBytes() const override { return base_->sizeInBytes() + 1; }
    size_t alignment() const override { return base_->alignment(); }
private:
    TypePtr base_;
};

class TupleType : public Type {
public:
    struct Element { std::string label; TypePtr type; };
    explicit TupleType(std::vector<Element> elems)
        : Type(TypeKind::Tuple), elems_(std::move(elems)) {}
    std::string name() const override;
    const std::vector<Element>& elements() const { return elems_; }
    size_t sizeInBytes() const override;
    size_t alignment() const override;
private:
    std::vector<Element> elems_;
};

class FunctionType : public Type {
public:
    struct Param { std::string label; TypePtr type; bool isInOut = false; };
    FunctionType(std::vector<Param> params, TypePtr ret, bool async = false, bool throws = false)
        : Type(TypeKind::Function), params_(std::move(params)), ret_(std::move(ret)),
          async_(async), throws_(throws) {}
    std::string name() const override;
    const std::vector<Param>& params() const { return params_; }
    TypePtr returnType() const { return ret_; }
    bool isAsync() const { return async_; }
    bool isThrows() const { return throws_; }
    size_t sizeInBytes() const override { return sizeof(void*) * 2; }
    size_t alignment() const override { return sizeof(void*); }
private:
    std::vector<Param> params_;
    TypePtr ret_;
    bool async_;
    bool throws_;
};

// 用户定义类型 / User-defined types
class StructType : public Type {
public:
    struct Field { std::string name; TypePtr type; size_t offset; };
    explicit StructType(std::string name) : Type(TypeKind::Struct), name_(std::move(name)) {}
    std::string name() const override { return name_; }
    void addField(const std::string& n, TypePtr t);
    const std::vector<Field>& fields() const { return fields_; }
    size_t sizeInBytes() const override;
    size_t alignment() const override;
private:
    std::string name_;
    std::vector<Field> fields_;
};

class ClassType : public Type {
public:
    explicit ClassType(std::string name) : Type(TypeKind::Class), name_(std::move(name)) {}
    std::string name() const override { return name_; }
    bool isReferenceType() const override { return true; }
    size_t sizeInBytes() const override { return sizeof(void*); }
    size_t alignment() const override { return sizeof(void*); }
private:
    std::string name_;
};

class EnumType : public Type {
public:
    struct Case { std::string name; std::vector<TypePtr> associatedTypes; };
    explicit EnumType(std::string name) : Type(TypeKind::Enum), name_(std::move(name)) {}
    std::string name() const override { return name_; }
    void addCase(const std::string& n, std::vector<TypePtr> assoc = {});
    const std::vector<Case>& cases() const { return cases_; }
    size_t sizeInBytes() const override;
    size_t alignment() const override;
private:
    std::string name_;
    std::vector<Case> cases_;
};

class ProtocolType : public Type {
public:
    explicit ProtocolType(std::string name) : Type(TypeKind::Protocol), name_(std::move(name)) {}
    std::string name() const override { return name_; }
    size_t sizeInBytes() const override { return sizeof(void*) * 2; } // witness table
    size_t alignment() const override { return sizeof(void*); }
private:
    std::string name_;
};

class ActorType : public Type {
public:
    explicit ActorType(std::string name) : Type(TypeKind::Actor), name_(std::move(name)) {}
    std::string name() const override { return name_; }
    bool isReferenceType() const override { return true; }
    size_t sizeInBytes() const override { return sizeof(void*); }
    size_t alignment() const override { return sizeof(void*); }
private:
    std::string name_;
};

class SetType : public Type {
public:
    explicit SetType(TypePtr elem) : Type(TypeKind::Set), elem_(std::move(elem)) {}
    std::string name() const override { return "Set<" + elem_->name() + ">"; }
    TypePtr elementType() const { return elem_; }
    size_t sizeInBytes() const override { return sizeof(void*) * 2; }
    size_t alignment() const override { return sizeof(void*); }
private:
    TypePtr elem_;
};

class OwnedType : public Type {
public:
    explicit OwnedType(TypePtr inner) : Type(TypeKind::Owned), inner_(std::move(inner)) {}
    std::string name() const override { return "Owned<" + inner_->name() + ">"; }
    TypePtr innerType() const { return inner_; }
    size_t sizeInBytes() const override { return inner_->sizeInBytes(); }
    size_t alignment() const override { return inner_->alignment(); }
private:
    TypePtr inner_;
};

class UIntType : public Type {
public:
    explicit UIntType(int bits = 64) : Type(TypeKind::UInt), bits_(bits) {}
    std::string name() const override { return "UInt"; }
    int bitWidth() const { return bits_; }
    size_t sizeInBytes() const override { return static_cast<size_t>(bits_ / 8); }
    size_t alignment() const override { return static_cast<size_t>(bits_ / 8); }
private:
    int bits_;
};

class CharType : public Type {
public:
    CharType() : Type(TypeKind::Char) {}
    std::string name() const override { return "Char"; }
    size_t sizeInBytes() const override { return 4; } // Unicode scalar
    size_t alignment() const override { return 4; }
};

class AnyType : public Type {
public:
    AnyType() : Type(TypeKind::Any) {}
    std::string name() const override { return "Any"; }
    size_t sizeInBytes() const override { return sizeof(void*) * 2; } // type + value
    size_t alignment() const override { return sizeof(void*); }
};

class AnyObjectType : public Type {
public:
    AnyObjectType() : Type(TypeKind::AnyObject) {}
    std::string name() const override { return "AnyObject"; }
    bool isReferenceType() const override { return true; }
    size_t sizeInBytes() const override { return sizeof(void*); }
    size_t alignment() const override { return sizeof(void*); }
};

class UnresolvedType : public Type {
public:
    explicit UnresolvedType(std::string name) : Type(TypeKind::Unresolved), name_(std::move(name)) {}
    std::string name() const override { return name_ + "?"; }
    size_t sizeInBytes() const override { return 0; }
    size_t alignment() const override { return 0; }
private:
    std::string name_;
};

class ErrorType : public Type {
public:
    ErrorType() : Type(TypeKind::Error) {}
    std::string name() const override { return "<error>"; }
    size_t sizeInBytes() const override { return 0; }
    size_t alignment() const override { return 0; }
};

// 类型工厂 / Type factories
TypePtr getVoidType();
TypePtr getBoolType();
TypePtr getIntType(int bits = 64);
TypePtr getUIntType(int bits = 64);
TypePtr getFloatType(int bits = 32);
TypePtr getDoubleType();
TypePtr getCharType();
TypePtr getStringType();
TypePtr getAnyType();
TypePtr getAnyObjectType();
TypePtr getErrorType();
TypePtr resolvePrimitiveType(const std::string& name);

} // namespace suki
