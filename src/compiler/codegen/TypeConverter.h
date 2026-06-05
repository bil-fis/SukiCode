#pragma once
// Converts SukiCode types to LLVM types.

#ifdef SUKI_HAS_LLVM
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>
#endif

namespace suki {

// Forward declarations
struct TypeRepr;

class TypeConverter {
public:
    // TODO: convert SukiCode type representations to LLVM types
};

} // namespace suki
