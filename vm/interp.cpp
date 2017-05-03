#include <cassert>
#include <iostream>
#include <unordered_map>
#include "runtime.h"
#include "parser.h"
#include "interp.h"
#include "core.h"

/// Inline cache to speed up property lookups
class ICache
{
private:

    // Cached slot index
    size_t slotIdx = 0;

    // Field name to look up
    std::string fieldName;

public:

    ICache(std::string fieldName)
    : fieldName(fieldName)
    {
    }

    Value getField(Object obj)
    {
        Value val;

        if (!obj.getField(fieldName.c_str(), val, slotIdx))
        {
            throw RunError("missing field \"" + fieldName + "\"");
        }

        return val;
    }

    int64_t getInt64(Object obj)
    {
        auto val = getField(obj);
        assert (val.isInt64());
        return (int64_t)val;
    }

    String getStr(Object obj)
    {
        auto val = getField(obj);
        assert (val.isString());
        return String(val);
    }

    Object getObj(Object obj)
    {
        auto val = getField(obj);
        assert (val.isObject());
        return Object(val);
    }

    Array getArr(Object obj)
    {
        auto val = getField(obj);
        assert (val.isArray());
        return Array(val);
    }
};

std::string posToString(Value srcPos)
{
    assert (srcPos.isObject());
    auto srcPosObj = (Object)srcPos;

    auto lineNo = (int64_t)srcPosObj.getField("line_no");
    auto colNo = (int64_t)srcPosObj.getField("col_no");
    auto srcName = (std::string)srcPosObj.getField("src_name");

    return (
        srcName + "@" +
        std::to_string(lineNo) + ":" +
        std::to_string(colNo)
    );
}

/// Opcode enumeration
enum Opcode : uint16_t
{
    GET_LOCAL,
    SET_LOCAL,

    // Stack manipulation
    PUSH,
    POP,
    DUP,
    SWAP,

    // 64-bit integer operations
    ADD_I64,
    SUB_I64,
    MUL_I64,
    LT_I64,
    LE_I64,
    GT_I64,
    GE_I64,
    EQ_I64,

    // String operations
    STR_LEN,
    GET_CHAR,
    GET_CHAR_CODE,
    STR_CAT,
    EQ_STR,

    // Object operations
    NEW_OBJECT,
    HAS_FIELD,
    SET_FIELD,
    GET_FIELD,
    EQ_OBJ,

    // Miscellaneous
    EQ_BOOL,
    HAS_TAG,
    GET_TAG,

    // Array operations
    NEW_ARRAY,
    ARRAY_LEN,
    ARRAY_PUSH,
    GET_ELEM,
    SET_ELEM,

    // Branch instructions
    // Note: opcode for stub branches is opcode+1
    JUMP,
    JUMP_STUB,
    IF_TRUE,
    CALL,
    RET,

    IMPORT,
    ABORT
};

/// Total count of instructions executed
size_t cycleCount = 0;

/// Cache of all possible one-character string values
Value charStrings[256];

/*
Value call(Object fun, ValueVec args)
{
    static ICache numParamsIC("num_params");
    static ICache numLocalsIC("num_locals");
    auto numParams = numParamsIC.getInt64(fun);
    auto numLocals = numLocalsIC.getInt64(fun);
    assert (args.size() <= numParams);
    assert (numParams <= numLocals);

    ValueVec locals;
    locals.resize(numLocals, Value::UNDEF);

    // Copy the arguments into the locals
    for (size_t i = 0; i < args.size(); ++i)
    {
        //std::cout << "  " << args[i].toString() << std::endl;
        locals[i] = args[i];
    }

    // Temporary value stack
    ValueVec stack;

    // Array of instructions to execute
    Value instrs;

    // Number of instructions in the current block
    size_t numInstrs = 0;

    // Index of the next instruction to execute
    size_t instrIdx = 0;

    auto popVal = [&stack]()
    {
        if (stack.empty())
            throw RunError("op cannot pop value, stack empty");
        auto val = stack.back();
        stack.pop_back();
        return val;
    };

    auto popBool = [&popVal]()
    {
        auto val = popVal();
        if (!val.isBool())
            throw RunError("op expects boolean value");
        return (bool)val;
    };

    auto popInt64 = [&popVal]()
    {
        auto val = popVal();
        if (!val.isInt64())
            throw RunError("op expects int64 value");
        return (int64_t)val;
    };

    auto popStr = [&popVal]()
    {
        auto val = popVal();
        if (!val.isString())
            throw RunError("op expects string value");
        return String(val);
    };

    auto popArray = [&popVal]()
    {
        auto val = popVal();
        if (!val.isArray())
            throw RunError("op expects array value");
        return Array(val);
    };

    auto popObj = [&popVal]()
    {
        auto val = popVal();
        assert (val.isObject());
        return Object(val);
    };

    auto pushBool = [&stack](bool val)
    {
        stack.push_back(val? Value::TRUE:Value::FALSE);
    };

    auto branchTo = [&instrs, &numInstrs, &instrIdx](Object targetBB)
    {
        //std::cout << "branching" << std::endl;

        if (instrIdx != numInstrs)
        {
            throw RunError(
                "only the last instruction in a block can be a branch ("
                "instrIdx=" + std::to_string(instrIdx) + ", " +
                "numInstrs=" + std::to_string(numInstrs) + ")"
            );
        }

        static ICache instrsIC("instrs");
        Array instrArr = instrsIC.getArr(targetBB);

        instrs = (Value)instrArr;
        numInstrs = instrArr.length();
        instrIdx = 0;

        if (numInstrs == 0)
        {
            throw RunError("target basic block is empty");
        }
    };

    // Get the entry block for this function
    static ICache entryIC("entry");
    Object entryBB = entryIC.getObj(fun);

    // Branch to the entry block
    branchTo(entryBB);

    // For each instruction to execute
    for (;;)
    {
        assert (instrIdx < numInstrs);

        //std::cout << "cycleCount=" << cycleCount << std::endl;
        //std::cout << "instrIdx=" << instrIdx << std::endl;

        Array instrArr = Array(instrs);
        Value instrVal = instrArr.getElem(instrIdx);
        assert (instrVal.isObject());
        auto instr = Object(instrVal);

        cycleCount++;
        instrIdx++;

        // Get the opcode for this instruction
        auto op = decode(instr);

        switch (op)
        {
            // Read a local variable and push it on the stack
            case GET_LOCAL:
            {
                static ICache icache("idx");
                auto localIdx = icache.getInt64(instr);
                //std::cout << "localIdx=" << localIdx << std::endl;
                assert (localIdx < locals.size());
                stack.push_back(locals[localIdx]);
            }
            break;

            // Set a local variable
            case SET_LOCAL:
            {
                static ICache icache("idx");
                auto localIdx = icache.getInt64(instr);
                //std::cout << "localIdx=" << localIdx << std::endl;
                assert (localIdx < locals.size());
                locals[localIdx] = popVal();
            }
            break;

            case PUSH:
            {
                static ICache icache("val");
                auto val = icache.getField(instr);
                stack.push_back(val);
            }
            break;

            case POP:
            {
                if (stack.empty())
                    throw RunError("pop failed, stack empty");
                stack.pop_back();
            }
            break;

            // Duplicate the top of the stack
            case DUP:
            {
                static ICache idxIC("idx");
                auto idx = idxIC.getInt64(instr);

                if (idx >= stack.size())
                    throw RunError("stack undeflow, invalid index for dup");

                auto val = stack[stack.size() - 1 - idx];
                stack.push_back(val);
            }
            break;

            // Swap the topmost two stack elements
            case SWAP:
            {
                auto v0 = popVal();
                auto v1 = popVal();
                stack.push_back(v0);
                stack.push_back(v1);
            }
            break;

            //
            // 64-bit integer operations
            //

            case ADD_I64:
            {
                auto arg1 = popInt64();
                auto arg0 = popInt64();
                stack.push_back(arg0 + arg1);
            }
            break;

            case SUB_I64:
            {
                auto arg1 = popInt64();
                auto arg0 = popInt64();
                stack.push_back(arg0 - arg1);
            }
            break;

            case MUL_I64:
            {
                auto arg1 = popInt64();
                auto arg0 = popInt64();
                stack.push_back(arg0 * arg1);
            }
            break;

            case EQ_I64:
            {
                auto arg1 = popInt64();
                auto arg0 = popInt64();
                pushBool(arg0 == arg1);
            }
            break;

            //
            // String operations
            //

            case STR_LEN:
            {
                auto str = popStr();
                stack.push_back(str.length());
            }
            break;

            case GET_CHAR:
            {
                auto idx = (size_t)popInt64();
                auto str = popStr();

                if (idx >= str.length())
                {
                    throw RunError(
                        "get_char, index out of bounds"
                    );
                }

                auto ch = str[idx];

                // Cache single-character strings
                if (charStrings[ch] == Value::FALSE)
                {
                    char buf[2] = { (char)str[idx], '\0' };
                    charStrings[ch] = String(buf);
                }

                stack.push_back(charStrings[ch]);
            }
            break;

            case GET_CHAR_CODE:
            {
                auto idx = (size_t)popInt64();
                auto str = popStr();

                if (idx >= str.length())
                {
                    throw RunError(
                        "get_char_code, index out of bounds"
                    );
                }

                stack.push_back((int64_t)str[idx]);
            }
            break;

            case STR_CAT:
            {
                auto a = popStr();
                auto b = popStr();
                auto c = String::concat(b, a);
                stack.push_back(c);
            }
            break;

            case EQ_STR:
            {
                auto arg1 = popStr();
                auto arg0 = popStr();
                pushBool(arg0 == arg1);
            }
            break;

            //
            // Object operations
            //

            case NEW_OBJECT:
            {
                auto capacity = popInt64();
                auto obj = Object::newObject(capacity);
                stack.push_back(obj);
            }
            break;

            case HAS_FIELD:
            {
                auto fieldName = popStr();
                auto obj = popObj();
                pushBool(obj.hasField(fieldName));
            }
            break;

            case SET_FIELD:
            {
                auto val = popVal();
                auto fieldName = popStr();
                auto obj = popObj();

                if (!isValidIdent(fieldName))
                {
                    throw RunError(
                        "invalid identifier in set_field \"" +
                        (std::string)fieldName + "\""
                    );
                }

                obj.setField(fieldName, val);
            }
            break;

            // This instruction will abort execution if trying to
            // access a field that is not present on an object.
            // The running program is responsible for testing that
            // fields exist before attempting to read them.
            case GET_FIELD:
            {
                auto fieldName = popStr();
                auto obj = popObj();

                //std::cout << "get " << std::string(fieldName) << std::endl;

                if (!obj.hasField(fieldName))
                {
                    throw RunError(
                        "get_field failed, missing field \"" +
                        (std::string)fieldName + "\""
                    );
                }

                auto val = obj.getField(fieldName);
                stack.push_back(val);
            }
            break;

            case EQ_OBJ:
            {
                Value arg1 = popVal();
                Value arg0 = popVal();
                pushBool(arg0 == arg1);
            }
            break;

            //
            // Array operations
            //

            case NEW_ARRAY:
            {
                auto len = popInt64();
                auto array = Array(len);
                stack.push_back(array);
            }
            break;

            case ARRAY_LEN:
            {
                auto arr = Array(popVal());
                stack.push_back(arr.length());
            }
            break;

            case ARRAY_PUSH:
            {
                auto val = popVal();
                auto arr = Array(popVal());
                arr.push(val);
            }
            break;

            case SET_ELEM:
            {
                auto val = popVal();
                auto idx = (size_t)popInt64();
                auto arr = Array(popVal());

                if (idx >= arr.length())
                {
                    throw RunError(
                        "set_elem, index out of bounds"
                    );
                }

                arr.setElem(idx, val);
            }
            break;

            case GET_ELEM:
            {
                auto idx = (size_t)popInt64();
                auto arr = Array(popVal());

                if (idx >= arr.length())
                {
                    throw RunError(
                        "get_elem, index out of bounds"
                    );
                }

                stack.push_back(arr.getElem(idx));
            }
            break;

            case EQ_BOOL:
            {
                auto arg1 = popBool();
                auto arg0 = popBool();
                pushBool(arg0 == arg1);
            }
            break;

            // Test if a value has a given tag
            case HAS_TAG:
            {
                auto tag = popVal().getTag();
                static ICache tagIC("tag");
                auto tagStr = tagIC.getStr(instr);

                switch (tag)
                {
                    case TAG_UNDEF:
                    pushBool(tagStr == "undef");
                    break;

                    case TAG_BOOL:
                    pushBool(tagStr == "bool");
                    break;

                    case TAG_INT64:
                    pushBool(tagStr == "int64");
                    break;

                    case TAG_STRING:
                    pushBool(tagStr == "string");
                    break;

                    case TAG_ARRAY:
                    pushBool(tagStr == "array");
                    break;

                    case TAG_OBJECT:
                    pushBool(tagStr == "object");
                    break;

                    default:
                    throw RunError(
                        "unknown value type in has_tag"
                    );
                }
            }
            break;

            // Regular function call
            case CALL:
            {
                static ICache retToCache("ret_to");
                static ICache numArgsCache("num_args");
                auto retToBB = retToCache.getObj(instr);
                auto numArgs = numArgsCache.getInt64(instr);

                auto callee = popVal();

                if (stack.size() < numArgs)
                {
                    throw RunError(
                        "stack underflow at call"
                    );
                }

                // Copy the arguments into a vector
                ValueVec args;
                args.resize(numArgs);
                for (size_t i = 0; i < numArgs; ++i)
                    args[numArgs - 1 - i] = popVal();

                static ICache numParamsIC("num_params");
                size_t numParams;
                if (callee.isObject())
                {
                    numParams = numParamsIC.getInt64(callee);
                }
                else if (callee.isHostFn())
                {
                    auto hostFn = (HostFn*)(callee.getWord().ptr);
                    numParams = hostFn->getNumParams();
                }
                else
                {
                    throw RunError("invalid callee at call site");
                }

                if (numArgs != numParams)
                {
                    std::string srcPosStr = (
                        instr.hasField("src_pos")?
                        (posToString(instr.getField("src_pos")) + " - "):
                        std::string("")
                    );

                    throw RunError(
                        srcPosStr +
                        "incorrect argument count in call, received " +
                        std::to_string(numArgs) +
                        ", expected " +
                        std::to_string(numParams)
                    );
                }

                Value retVal;

                if (callee.isObject())
                {
                    // Perform the call
                    retVal = call(callee, args);
                }
                else if (callee.isHostFn())
                {
                    auto hostFn = (HostFn*)(callee.getWord().ptr);

                    // Call the host function
                    switch (numArgs)
                    {
                        case 0:
                        retVal = hostFn->call0();
                        break;

                        case 1:
                        retVal = hostFn->call1(args[0]);
                        break;

                        case 2:
                        retVal = hostFn->call2(args[0], args[1]);
                        break;

                        case 3:
                        retVal = hostFn->call3(args[0], args[1], args[2]);
                        break;

                        default:
                        assert (false);
                    }
                }

                // Push the return value on the stack
                stack.push_back(retVal);

                // Jump to the return basic block
                branchTo(retToBB);
            }
            break;

            case RET:
            {
                auto val = stack.back();
                stack.pop_back();
                return val;
            }
            break;

            case IMPORT:
            {
                auto pkgName = popStr();
                auto pkg = import(pkgName);
                stack.push_back(pkg);
            }
            break;

            case ABORT:
            {
                auto errMsg = (std::string)popStr();

                // If a source position was specified
                if (instr.hasField("src_pos"))
                {
                    auto srcPos = instr.getField("src_pos");
                    std::cout << posToString(srcPos) << " - ";
                }

                if (errMsg != "")
                {
                    std::cout << "aborting execution due to error: ";
                    std::cout << errMsg << std::endl;
                }
                else
                {
                    std::cout << "aborting execution due to error" << std::endl;
                }

                exit(-1);
            }
            break;

            default:
            assert (false && "unhandled op in interpreter");
        }
    }

    assert (false);
}
*/

/// Initial code heap size in bytes
const size_t CODE_HEAP_INIT_SIZE = 1 << 20;

/// Initial stack size in words
const size_t STACK_INIT_SIZE = 1 << 16;

class CodeFragment
{
public:

    /// Start index in the executable heap
    uint8_t* startPtr = nullptr;

    /// End index in the executable heap
    uint8_t* endPtr = nullptr;

    /// Get the length of the code fragment
    size_t length()
    {
        assert (startPtr);
        assert (endPtr);
        return endPtr - startPtr;
    }
};

class BlockVersion : public CodeFragment
{
public:

    /// Associated block
    Object block;

    /// Code generation context at block entry
    //CodeGenCtx ctx;

    BlockVersion(Object block)
    : block(block)
    {
    }
};

typedef std::vector<BlockVersion*> VersionList;

/// Flat array of bytes into which code gets compiled
uint8_t* codeHeap = nullptr;

/// Limit pointer for the code heap
uint8_t* codeHeapLimit = nullptr;

/// Current allocation pointer in the code heap
uint8_t* codeHeapAlloc = nullptr;

/// Map of block objects to lists of versions
std::unordered_map<refptr, VersionList> versionMap;

/// Size of the stack in words
size_t stackSize = 0;

/// Lower stack limit (stack pointer must be greater than this)
Value* stackLimit = nullptr;

/// Stack bottom (end of the stack memory array)
Value* stackBottom = nullptr;

/// Stack frame base pointer
Value* basePtr = nullptr;

/// Current temp stack top pointer
Value* stackPtr = nullptr;

// Current instruction pointer
uint8_t* instrPtr = nullptr;

/// Write a value to the code heap
template <typename T> void writeCode(T val)
{
    assert (codeHeapAlloc < codeHeapLimit);
    T* heapPtr = (T*)codeHeapAlloc;
    *heapPtr = val;
    codeHeapAlloc += sizeof(T);
    assert (codeHeapAlloc <= codeHeapLimit);
}

/// Return a pointer to a value to read from the code stream
template <typename T> __attribute__((always_inline)) T& readCode()
{
    assert (instrPtr + sizeof(T) <= codeHeapLimit);
    T* valPtr = (T*)instrPtr;
    instrPtr += sizeof(T);
    return *valPtr;
}

/// Push a value on the stack
__attribute__((always_inline)) void pushVal(Value val)
{
    assert (stackPtr > stackLimit);
    stackPtr--;
    stackPtr[0] = val;
}

__attribute__((always_inline)) Value popVal()
{
    assert (stackPtr < stackBottom);
    auto val = stackPtr[0];
    stackPtr++;
    return val;
}

/// Initialize the interpreter
void initInterp()
{
    // Allocate the code heap
    codeHeap = new uint8_t[CODE_HEAP_INIT_SIZE];
    codeHeapLimit = codeHeap + CODE_HEAP_INIT_SIZE;
    codeHeapAlloc = codeHeap;

    // Allocate the stack
    stackSize = STACK_INIT_SIZE;
    stackLimit = new Value[STACK_INIT_SIZE];
    stackBottom = stackLimit + sizeof(Word);
    stackPtr = stackBottom;
}

/// Get a version of a block. This version will be a stub
/// until compiled
BlockVersion* getBlockVersion(Object block)
{
    auto blockPtr = (refptr)block;

    auto versionItr = versionMap.find((refptr)block);

    if (versionItr == versionMap.end())
    {
        versionMap[blockPtr] = VersionList();
    }
    else
    {
        auto versions = versionItr->second;
        assert (versions.size() == 1);
        return versions[0];
    }

    auto newVersion = new BlockVersion(block);

    auto& versionList = versionMap[blockPtr];
    versionList.push_back(newVersion);

    return newVersion;
}

void compile(BlockVersion* version)
{
    std::cout << "compiling version" << std::endl;

    auto block = version->block;

    // Get the instructions array
    static ICache instrsIC("instrs");
    Array instrs = instrsIC.getArr(block);

    // Mark the block start
    version->startPtr = codeHeapAlloc;

    // For each instruction
    for (size_t i = 0; i < instrs.length(); ++i)
    {
        auto instrVal = instrs.getElem(i);
        assert (instrVal.isObject());
        auto instr = (Object)instrVal;

        static ICache opIC("op");
        auto op = (std::string)opIC.getStr(instr);

        std::cout << "op: " << op << std::endl;

        if (op == "push")
        {
            static ICache valIC("val");
            auto val = valIC.getField(instr);
            writeCode(PUSH);
            writeCode(val);
            continue;
        }

        if (op == "dup")
        {
            static ICache idxIC("idx");
            auto idx = (uint16_t)idxIC.getInt64(instr);
            writeCode(DUP);
            writeCode(idx);
            continue;
        }

        if (op == "sub_i64")
        {
            writeCode(SUB_I64);
            continue;
        }

        if (op == "lt_i64")
        {
            writeCode(LT_I64);
            continue;
        }

        if (op == "gt_i64")
        {
            writeCode(GT_I64);
            continue;
        }

        if (op == "jump")
        {
            static ICache toIC("to");
            auto dstBB = toIC.getObj(instr);

            auto dstVer = getBlockVersion(dstBB);

            writeCode(JUMP_STUB);
            writeCode(dstVer);

            continue;
        }

        if (op == "if_true")
        {
            static ICache thenIC("then");
            static ICache elseIC("else");
            auto thenBB = thenIC.getObj(instr);
            auto elseBB = elseIC.getObj(instr);

            auto thenVer = getBlockVersion(thenBB);
            auto elseVer = getBlockVersion(elseBB);

            writeCode(IF_TRUE);
            writeCode(thenVer);
            writeCode(elseVer);

            continue;
        }

        if (op == "ret")
        {
            writeCode(RET);
            continue;
        }

        throw RunError("unhandled opcode in basic block \"" + op + "\"");
    }

    // Mark the block end
    version->endPtr = codeHeapAlloc;
}

/// Start/continue execution beginning at a current instruction
Value execCode()
{
    assert (instrPtr >= codeHeap);
    assert (instrPtr < codeHeapLimit);

    // For each instruction to execute
    for (;;)
    {
        auto& op = readCode<Opcode>();

        switch (op)
        {
            case PUSH:
            {
                auto val = readCode<Value>();
                pushVal(val);
            }
            break;

            case DUP:
            {
                // Read the index of the value to duplicate
                auto idx = readCode<uint16_t>();
                auto val = stackPtr[idx];
                pushVal(val);
            }
            break;

            case SUB_I64:
            {
                auto arg1 = popVal();
                auto arg0 = popVal();
                pushVal((int64_t)arg0 - (int64_t)arg1);
            }
            break;

            case LT_I64:
            {
                auto arg1 = popVal();
                auto arg0 = popVal();
                auto boolVal = (int64_t)arg0 < (int64_t)arg1;
                pushVal(boolVal? Value::TRUE : Value::FALSE);
            }
            break;

            case GT_I64:
            {
                auto arg1 = popVal();
                auto arg0 = popVal();
                auto boolVal = (int64_t)arg0 > (int64_t)arg1;
                pushVal(boolVal? Value::TRUE : Value::FALSE);
            }
            break;

            case JUMP_STUB:
            {
                auto& dstAddr = readCode<uint8_t*>();

                std::cout << "Patching jump" << std::endl;

                auto dstVer = (BlockVersion*)dstAddr;

                if (!dstVer->startPtr)
                    compile(dstVer);

                // Patch the jump
                op = JUMP;
                dstAddr = dstVer->startPtr;

                // Jump to the target
                instrPtr = dstVer->startPtr;
            }
            break;

            case JUMP:
            {
                auto& dstAddr = readCode<uint8_t*>();
                instrPtr = dstAddr;
            }
            break;

            case IF_TRUE:
            {
                auto& thenAddr = readCode<uint8_t*>();
                auto& elseAddr = readCode<uint8_t*>();

                auto arg0 = popVal();

                if (arg0 == Value::TRUE)
                {
                    if (thenAddr < codeHeap || thenAddr >= codeHeapLimit)
                    {
                        std::cout << "Patching then target" << std::endl;

                        auto thenVer = (BlockVersion*)thenAddr;
                        if (!thenVer->startPtr)
                           compile(thenVer);

                        // Patch the jump
                        thenAddr = thenVer->startPtr;
                    }

                    instrPtr = thenAddr;
                }
                else
                {
                    if (elseAddr < codeHeap || elseAddr >= codeHeapLimit)
                    {
                       std::cout << "Patching else target" << std::endl;

                       auto elseVer = (BlockVersion*)elseAddr;
                       if (!elseVer->startPtr)
                           compile(elseVer);

                       // Patch the jump
                       elseAddr = elseVer->startPtr;
                    }

                    instrPtr = elseAddr;
                }
            }
            break;

            case RET:
            {
                // TODO:
                // Get the caller function, base pointer adjustment

                // Get the return address
                auto retAddrVal = basePtr[1];
                auto retAddr = retAddrVal.getWord().ptr;

                // Pop the return value
                auto val = popVal();

                // If this is a top-level return
                if (retAddr == nullptr)
                {
                    return val;
                }
                else
                {
                    // TODO
                    assert (false);
                }
            }
            break;

            default:
            assert (false && "unhandled instruction in interpreter loop");
        }

    }

    assert (false);
}

/// Begin the execution of a function (top-level call)
Value callFun(Object fun, ValueVec args)
{
    static ICache numParamsIC("num_params");
    static ICache numLocalsIC("num_locals");
    auto numParams = numParamsIC.getInt64(fun);
    auto numLocals = numLocalsIC.getInt64(fun);
    assert (args.size() <= numParams);
    assert (numParams <= numLocals);

    std::cout << "pushing RA" << std::endl;

    // Push the caller function and return address
    // Note: these are placeholders because we are doing a toplevel call
    assert (stackPtr == stackBottom);
    pushVal(Value(0));
    pushVal(Value(nullptr, TAG_RETADDR));

    // Initialize the base pointer (used to access locals)
    basePtr = stackPtr - 1;

    // Push space for the local variables
    stackPtr -= numLocals;
    assert (stackPtr >= stackLimit);

    std::cout << "pushing locals" << std::endl;

    // Copy the arguments into the locals
    for (size_t i = 0; i < args.size(); ++i)
    {
        //std::cout << "  " << args[i].toString() << std::endl;
        basePtr[i] = args[i];
    }

    // Get the function entry block
    static ICache entryIC("entry");
    auto entryBlock = entryIC.getObj(fun);

    auto entryVer = getBlockVersion(entryBlock);

    // Generate code for the entry block version
    compile(entryVer);
    assert (entryVer->length() > 0);

    std::cout << "Starting top-level unit execution" << std::endl;

    // Begin execution at the entry block
    instrPtr = entryVer->startPtr;
    auto retVal = execCode();

    // Pop the local variables, return address and calling function
    stackPtr += numLocals;
    stackPtr += 2;
    assert (stackPtr == stackBottom);

    return retVal;
}

/// Call a function exported by a package
Value callExportFn(
    Object pkg,
    std::string fnName,
    ValueVec args
)
{
    assert (pkg.hasField(fnName));
    auto fnVal = pkg.getField(fnName);
    assert (fnVal.isObject());
    auto funObj = Object(fnVal);

    return callFun(funObj, args);
}

Value testRunImage(std::string fileName)
{
    std::cout << "loading image \"" << fileName << "\"" << std::endl;

    auto pkg = parseFile(fileName);

    return callExportFn(pkg, "main");
}

void testInterp()
{
    assert (testRunImage("tests/vm/ex_ret_cst.zim") == Value(777));
    assert (testRunImage("tests/vm/ex_loop_cnt.zim") == Value(0));
    //assert (testRunImage("tests/vm/ex_image.zim") == Value(10));
    //assert (testRunImage("tests/vm/ex_rec_fact.zim") == Value(5040));
    //assert (testRunImage("tests/vm/ex_fibonacci.zim") == Value(377));
}
