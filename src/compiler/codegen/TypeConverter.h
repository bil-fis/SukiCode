#pragma once
// SukiCode 类型到 LLVM 类型的转换器
// Converts SukiCode types to LLVM IR types.

#ifdef SUKI_HAS_LLVM
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>
#endif

#include "compiler/sema/Type.h"
#include <unordered_map>

namespace suki {

class TypeConverter {
public:
#ifdef SUKI_HAS_LLVM
    explicit TypeConverter(llvm::LLVMContext& ctx) : ctx_(ctx) {}

    // Convert a SukiCode Type to an LLVM Type
    llvm::Type* convert(const Type& type);

    // Get LLVM void type
    llvm::Type* getVoidType() { return llvm::Type::getVoidTy(ctx_); }

    // Get LLVM i1 (bool) type
    llvm::Type* getBoolType() { return llvm::Type::getInt1Ty(ctx_); }

    // Get LLVM integer type
    llvm::Type* getIntType(int bits) { return llvm::Type::getIntNTy(ctx_, bits); }

    // Get LLVM float/double type
    llvm::Type* getFloatType() { return llvm::Type::getFloatTy(ctx_); }
    llvm::Type* getDoubleType() { return llvm::Type::getDoubleTy(ctx_); }

    // Get LLVM pointer type
    llvm::PointerType* getPtrType() { return llvm::PointerType::get(ctx_, 0); }

    // Get LLVM string type (struct { i8*, i64 })
    llvm::StructType* getStringType();

    // Get LLVM array type
    llvm::ArrayType* getArrayType(llvm::Type* elemType, uint64_t count);

private:
    llvm::LLVMContext& ctx_;
    std::unordered_map<const Type*, llvm::Type*> cache_;
#endif
};

} // namespace suki
