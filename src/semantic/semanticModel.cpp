#include "semanticModel.h"

namespace bob::sema {

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

ClassInfo* SemanticModel::classNamed(std::string_view name) const
{
    for (const auto& info : classes) {
        if (info->name == name) {
            return info.get();
        }
    }
    return nullptr;
}

const Type* SemanticModel::typeOf(const ast::Node* node) const
{
    auto it = node != nullptr ? expressionTypes.find(node->id) : expressionTypes.end();
    return it == expressionTypes.end() ? nullptr : it->second;
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
    for (const auto& record : records) {
        if (record->name == name) {
            return record.get();
        }
    }
    return nullptr;
}

}
