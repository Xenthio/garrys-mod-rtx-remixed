#pragma once
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace GarrysMod { namespace Lua {
namespace Type { enum { Nil, Bool, Number, String, Table, Function }; }
constexpr int SPECIAL_GLOB = 0;
class ILuaBase;
using CFunction = int (*)(ILuaBase*);

struct Value {
    int type = Type::Nil;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::shared_ptr<std::map<std::string, Value>> table;
    CFunction function = nullptr;
};

// Minimal stack implementation. It executes the actual registration and Lua
// callbacks without loading Source or a graphics device.
class ILuaBase {
public:
    ILuaBase() { globals.type = Type::Table; globals.table = std::make_shared<std::map<std::string, Value>>(); }
    Value globals;
    std::vector<Value> stack;

    Value& At(int index) {
        const auto offset = index > 0 ? static_cast<size_t>(index - 1) : stack.size() + index;
        return stack.at(offset);
    }
    void PushSpecial(int value) {
        if (value != SPECIAL_GLOB) throw std::runtime_error("unexpected Lua special table");
        stack.push_back(globals);
    }
    void GetField(int index, const char* name) { const auto value = At(index).table->find(name); stack.push_back(value == At(index).table->end() ? Value{} : value->second); }
    void SetField(int index, const char* name) { auto table = At(index).table; (*table)[name] = stack.back(); stack.pop_back(); }
    void Pop(int count = 1) { while (count-- > 0) stack.pop_back(); }
    bool IsType(int index, int type) { return At(index).type == type; }
    void CreateTable() { Value value; value.type = Type::Table; value.table = std::make_shared<std::map<std::string, Value>>(); stack.push_back(value); }
    void PushNumber(double number) { Value value; value.type = Type::Number; value.number = number; stack.push_back(value); }
    void PushBool(bool boolean) { Value value; value.type = Type::Bool; value.boolean = boolean; stack.push_back(value); }
    void PushString(const char* bytes, unsigned int length = 0) { Value value; value.type = Type::String; value.string = length ? std::string(bytes, length) : std::string(bytes); stack.push_back(value); }
    void PushNil() { stack.push_back({}); }
    void PushCFunction(CFunction function) { Value value; value.type = Type::Function; value.function = function; stack.push_back(value); }
    const char* GetString(int index, unsigned int* length = nullptr) { auto& value = At(index); if (length) *length = static_cast<unsigned int>(value.string.size()); return value.string.c_str(); }
    int PCall(int arguments, int results, int errorFunction) {
        if (arguments != 1 || results != 0 || errorFunction != 0) throw std::runtime_error("unexpected protected-call contract");
        const auto function = At(-2).function;
        const auto argument = stack.back();
        stack.resize(stack.size() - 2);
        auto caller = stack;
        stack = {argument};
        try {
            function(this);
            stack = caller;
            return 0;
        } catch (const std::exception& error) {
            stack = caller;
            PushString(error.what());
            return 1;
        }
    }
};
} }

#define LUA_FUNCTION(name) int name(GarrysMod::Lua::ILuaBase* LUA)
