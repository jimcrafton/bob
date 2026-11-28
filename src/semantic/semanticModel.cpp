#include "semanticModel.h"

#include <algorithm>

namespace bob::sema {

const char* reflectLevelName(ReflectLevel level)
{
    switch (level) {
    case ReflectLevel::none:
        return "none";
    case ReflectLevel::identity:
        return "identity";
    case ReflectLevel::introspect:
        return "introspect";
    case ReflectLevel::invoke:
        return "invoke";
    }
    return "introspect";
}

bool parseReflectLevel(std::string_view name, ReflectLevel& level)
{
    for (ReflectLevel candidate : {ReflectLevel::none, ReflectLevel::identity, ReflectLevel::introspect, ReflectLevel::invoke}) {
        if (name == reflectLevelName(candidate)) {
            level = candidate;
            return true;
        }
    }
    return false;
}

namespace {

// The unit's own declaration first (it shadows imports), then the first imported
// one. `module.Name` looks only in that module.
template <class T>
T* findNamed(const std::vector<std::unique_ptr<T>>& list, std::string_view name)
{
    std::string_view module;
    size_t dot = name.rfind('.');
    if (dot != std::string_view::npos) {
        module = name.substr(0, dot);
        name = name.substr(dot + 1);
    }
    T* imported = nullptr;
    for (const auto& item : list) {
        if (item->name != name) {
            continue;
        }
        const ModuleInfo* origin = item->origin.module;
        if (!module.empty()) {
            if (origin != nullptr && origin->name == module) {
                return item.get();
            }
            continue;
        }
        if (origin == nullptr) {
            return item.get();
        }
        imported = imported != nullptr ? imported : item.get();
    }
    return imported;
}

}

const FieldInfo* RecordInfo::field(std::string_view fieldName) const
{
    for (const FieldInfo& info : fields) {
        if (info.name == fieldName) {
            return &info;
        }
    }
    return nullptr;
}

std::string RecordInfo::constructorSelector() const
{
    std::string selector;
    for (const FieldInfo& info : fields) {
        selector += info.name + ":";
    }
    return selector;
}

const FieldInfo* ClassInfo::field(std::string_view fieldName) const
{
    for (const FieldInfo& info : fields) {
        if (info.name == fieldName) {
            return &info;
        }
    }
    return nullptr;
}

bool ClassInfo::isKindOf(const ClassInfo* other) const
{
    if (other == this) {
        return true;
    }
    for (const ClassInfo* protocol : protocols) {
        if (protocol->isKindOf(other)) {
            return true;
        }
    }
    return superclass != nullptr && superclass->isKindOf(other);
}

MessageInfo* ClassInfo::lookup(const std::string& selector) const
{
    auto it = methods.find(selector);
    if (it != methods.end()) {
        return it->second;
    }
    if (superclass != nullptr) {
        if (MessageInfo* inherited = superclass->lookup(selector)) {
            return inherited;
        }
    }
    for (const ClassInfo* protocol : protocols) {
        if (MessageInfo* declared = protocol->lookup(selector)) {
            return declared;
        }
    }
    return nullptr;
}

int ClassInfo::slotOf(const std::string& selector) const
{
    for (size_t i = 0; i < slotSelectors.size(); ++i) {
        if (slotSelectors[i] == selector) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<std::string> ClassInfo::witnessSelectors() const
{
    std::vector<std::string> slots;
    auto add = [&](const std::string& selector) {
        if (std::find(slots.begin(), slots.end(), selector) == slots.end()) {
            slots.push_back(selector);
        }
    };
    for (const ClassInfo* refined : protocols) {
        for (const std::string& selector : refined->witnessSelectors()) {
            add(selector);
        }
    }
    for (const MessageInfo* message : methodList) {
        add(message->selector);
    }
    return slots;
}

int ClassInfo::witnessSlotOf(const std::string& selector) const
{
    std::vector<std::string> slots = witnessSelectors();
    auto it = std::find(slots.begin(), slots.end(), selector);
    return it == slots.end() ? -1 : static_cast<int>(it - slots.begin());
}

std::vector<const ClassInfo*> ClassInfo::conformedProtocols() const
{
    std::vector<const ClassInfo*> result;
    std::vector<const ClassInfo*> pending;
    for (const ClassInfo* info = this; info != nullptr; info = info->superclass) {
        pending.insert(pending.end(), info->protocols.begin(), info->protocols.end());
    }
    while (!pending.empty()) {
        const ClassInfo* protocol = pending.front();
        pending.erase(pending.begin());
        if (std::find(result.begin(), result.end(), protocol) != result.end()) {
            continue;
        }
        result.push_back(protocol);
        pending.insert(pending.end(), protocol->protocols.begin(), protocol->protocols.end());
    }
    return result;
}

ClassInfo* SemanticModel::classNamed(std::string_view name) const
{
    return findNamed(classes, name);
}

ConstantInfo* SemanticModel::constantNamed(std::string_view name) const
{
    return findNamed(constants, name);
}

EnumInfo* SemanticModel::enumNamed(std::string_view name) const
{
    return findNamed(enums, name);
}

const NamedTypeInfo* SemanticModel::namedType(std::string_view name) const
{
    return findNamed(namedTypes, name);
}

const EnumMemberInfo* EnumInfo::member(std::string_view memberName) const
{
    for (const EnumMemberInfo& m : members) {
        if (m.name == memberName) {
            return &m;
        }
    }
    return nullptr;
}

const EnumMemberInfo* EnumInfo::memberWithValue(uint64_t value) const
{
    for (const EnumMemberInfo& m : members) {
        if (m.value == value) {
            return &m;
        }
    }
    return nullptr;
}

// Pending containers whose element type was inferred later read as the final type.
const Type* SemanticModel::typeOf(const ast::Node* node) const
{
    auto it = node != nullptr ? expressionTypes.find(node->id) : expressionTypes.end();
    return it == expressionTypes.end() ? nullptr : canonical(it->second);
}

const LocalInfo* SemanticModel::localOf(const ast::Node* node) const
{
    auto it = node != nullptr ? localRefs.find(node->id) : localRefs.end();
    return it == localRefs.end() ? nullptr : it->second;
}

const SendInfo* SemanticModel::sendOf(const ast::Node* node) const
{
    auto it = node != nullptr ? sends.find(node->id) : sends.end();
    return it == sends.end() ? nullptr : &it->second;
}

const FieldInfo* SemanticModel::fieldOf(const ast::Node* node) const
{
    auto it = node != nullptr ? fieldRefs.find(node->id) : fieldRefs.end();
    return it == fieldRefs.end() ? nullptr : it->second;
}

const BlockInfo* SemanticModel::blockOf(const ast::Node* node) const
{
    auto it = node != nullptr ? blocks.find(node->id) : blocks.end();
    return it == blocks.end() ? nullptr : it->second;
}

const RecordInfo* SemanticModel::recordNamed(std::string_view name) const
{
    return findNamed(records, name);
}

ModuleInfo* SemanticModel::moduleNamed(std::string_view name) const
{
    for (const auto& module : modules) {
        if (module->name == name) {
            return module.get();
        }
    }
    return nullptr;
}

}
