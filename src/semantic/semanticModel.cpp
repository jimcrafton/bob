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
