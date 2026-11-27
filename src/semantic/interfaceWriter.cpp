// Writes a module's interface file (design §11.5): its `interface { }`
// declarations as bob source without bodies, read back by importers with the
// ordinary parser and analyzer. Every name a declaration refers to is
// qualified with its module, so an importer resolves it the same way whatever
// else it imports.

#include "analyzer.h"

#include <cinttypes>
#include <cstdio>
#include <functional>
#include <sstream>

namespace bob::sema {

namespace {

class InterfaceWriter {
public:
    InterfaceWriter(const ast::Unit* unit, const SemanticModel& model) : unit_(unit), model_(model) {}

    std::string body()
    {
        for (const ast::Import* import : unit_->imports) {
            const ModuleInfo* module = model_.moduleNamed(import->name);
            out_ << "import " << import->name;
            if (module != nullptr && !module->version.empty()) {
                out_ << ", " << module->version;
            }
            out_ << ";\n";
        }
        out_ << "{\n";
        for (const auto& record : model_.records) {
            if (record->origin.exported) {
                writeRecord(*record);
            }
        }
        for (const auto& info : model_.enums) {
            if (info->origin.exported) {
                writeEnum(*info);
            }
        }
        for (const auto& named : model_.namedTypes) {
            if (named->origin.exported) {
                writeNamedType(*named);
            }
        }
        for (const auto& info : model_.classes) {
            if (info->origin.exported) {
                writeClass(*info);
            }
        }
        for (const auto& constant : model_.constants) {
            if (constant->origin.exported) {
                writeConstant(*constant);
            }
        }
        std::ostringstream messages;
        for (const MessageInfo* message : model_.program.messages) {
            if (message->origin.exported) {
                messages << "        " << signature(*message, true) << ";\n";
            }
        }
        if (!messages.str().empty()) {
            out_ << "    extends " << unit_->name << " {\n" << messages.str() << "    }\n";
        }
        out_ << "}\n";
        return out_.str();
    }

private:
    std::string moduleOf(const Origin& origin) const { return origin.module != nullptr ? origin.module->name : unit_->name; }

    std::string spell(const Type* type, bool grouped) const
    {
        type = canonical(type);
        std::string text = spellBare(type);
        return grouped && text.find(' ') != std::string::npos ? "[" + text + "]" : text;
    }

    const NamedTypeInfo* namedFor(const Type* type) const
    {
        for (const auto& named : model_.namedTypes) {
            if (named->type == type) {
                return named.get();
            }
        }
        return nullptr;
    }

    std::string spellBare(const Type* type) const
    {
        if (const NamedTypeInfo* named = (type->distinctOf != nullptr || isSubrange(type)) ? namedFor(type) : nullptr) {
            return moduleOf(named->origin) + "." + named->name;
        }
        switch (type->kind) {
        case TypeKind::record:
            return moduleOf(type->record->origin) + "." + type->record->name;
        case TypeKind::classType:
            return moduleOf(type->classInfo->origin) + "." + type->classInfo->name;
        case TypeKind::enumType:
            return moduleOf(type->enumInfo->origin) + "." + type->enumInfo->name;
        case TypeKind::optional:
            return spell(type->inner, true) + "?";
        case TypeKind::array:
            return "array of: " + spell(type->inner, true);
        case TypeKind::fixedArray:
            return "array of: " + spell(type->inner, true) + " by: " + spell(type->key, true);
        case TypeKind::dictionary:
            return "dictionary of: " + spell(type->inner, true);
        case TypeKind::hashtable:
            return "hashtable keys: " + spell(type->key, true) + " values: " + spell(type->inner, true);
        case TypeKind::set:
            return "set of: " + spell(type->inner, true);
        case TypeKind::subrange:
            if (isEnum(type->inner)) {
                std::string enumName = spell(type->inner, false);
                const EnumInfo* info = type->inner->enumInfo;
                return enumName + " " + info->members[static_cast<size_t>(type->low)].name + " to: " + enumName + " " +
                       info->members[static_cast<size_t>(type->high)].name;
            }
            return std::to_string(type->low) + " to: " + std::to_string(type->high);
        case TypeKind::block: {
            std::string text = "block";
            if (!type->params.empty()) {
                text += " takes: [";
                for (size_t i = 0; i < type->params.size(); ++i) {
                    text += (i == 0 ? "" : ", ") + spell(type->params[i], false);
                }
                text += "]";
            }
            if (type->result != nullptr && type->result->kind != TypeKind::voidType) {
                text += " returns: " + spell(type->result, true);
            }
            return text;
        }
        default:
            return type->name;
        }
    }

    static std::string quote(const std::string& text)
    {
        std::string out = "\"";
        for (unsigned char c : text) {
            switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\r':
                out += "\\r";
                break;
            default:
                if (c < 0x20) {
                    char escape[16];
                    std::snprintf(escape, sizeof(escape), "\\u{%X}", c);
                    out += escape;
                }
                else {
                    out += static_cast<char>(c);
                }
            }
        }
        return out + "\"";
    }

    std::string literal(const ConstValue& value, const Type* type) const
    {
        type = canonical(type);
        switch (type->kind) {
        case TypeKind::boolType:
            return value.bits != 0 ? "true" : "false";
        case TypeKind::integer:
        case TypeKind::subrange:
            if (isSubrange(type) && isEnum(type->inner)) {
                return literal(value, type->inner);
            }
            if (type->isSigned) {
                int64_t number = static_cast<int64_t>(value.bits << (64 - type->bits)) >> (64 - type->bits);
                return std::to_string(number);
            }
            return std::to_string(type->bits == 64 ? value.bits : value.bits & ((uint64_t(1) << type->bits) - 1));
        case TypeKind::enumType: {
            const EnumMemberInfo* member = type->enumInfo->memberWithValue(value.bits);
            return spell(type, false) + " " + (member != nullptr ? member->name : "?");
        }
        case TypeKind::character: {
            char text[24];
            std::snprintf(text, sizeof(text), "'\\u{%X}'", static_cast<unsigned>(value.bits));
            return text;
        }
        case TypeKind::real: {
            char text[48];
            std::snprintf(text, sizeof(text), "%.17g", value.real);
            std::string out = text;
            if (out.find_first_of(".en") == std::string::npos) {
                out += ".0";
            }
            return out;
        }
        case TypeKind::string:
            return quote(value.text);
        case TypeKind::record: {
            std::string out = "(" + spell(type, false);
            for (size_t i = 0; i < type->record->fields.size(); ++i) {
                out += " " + type->record->fields[i].name + ": " + literal(value.elements[i], type->record->fields[i].type);
            }
            return out + ")";
        }
        case TypeKind::optional:
            return value.present ? literal(value.elements[0], type->inner) : "nil";
        case TypeKind::set:
            if (isBitSet(type)) {
                std::string out = "[";
                for (const EnumMemberInfo& member : type->inner->enumInfo->members) {
                    if ((value.bits >> member.value) & 1) {
                        out += (out.size() == 1 ? "" : ", ") + spell(type->inner, false) + " " + member.name;
                    }
                }
                return out + "]";
            }
            [[fallthrough]];
        case TypeKind::array:
        case TypeKind::fixedArray: {
            std::string out = "[";
            for (size_t i = 0; i < value.elements.size(); ++i) {
                out += (i == 0 ? "" : ", ") + literal(value.elements[i], type->inner);
            }
            return out + "]";
        }
        default:
            return "nil";
        }
    }

    std::string signature(const MessageInfo& message, bool isProgram) const
    {
        std::vector<std::string> attributes;
        if (isProgram || (message.owner != nullptr && !message.isInstance)) {
            attributes.push_back("static");
        }
        if (message.compileTimeOnly) {
            attributes.push_back("compiletime");
        }
        if (message.isProtocolDefault) {
            attributes.push_back("default");
        }
        std::string keeps;
        if (message.selfEscapes) {
            keeps = "self";
        }
        for (size_t i = 0; i < message.params.size() && i < message.paramEscapes.size(); ++i) {
            if (message.paramEscapes[i]) {
                keeps += (keeps.empty() ? "" : " ") + message.params[i]->name;
            }
        }
        if (!keeps.empty()) {
            attributes.push_back("keeps: \"" + keeps + "\"");
        }
        std::string out;
        if (!attributes.empty()) {
            out += "@[";
            for (size_t i = 0; i < attributes.size(); ++i) {
                out += (i == 0 ? "" : ", ") + attributes[i];
            }
            out += "] ";
        }
        out += "msg ";
        const ast::MessageDecl* decl = message.decl;
        if (decl->messageKind == ast::MessageKind::unary) {
            out += decl->selector;
        }
        else if (decl->messageKind == ast::MessageKind::binary) {
            out += decl->selector + " " + message.params[0]->name + ":" + spell(message.params[0]->type, true);
        }
        else {
            for (size_t i = 0; i < decl->params.size(); ++i) {
                auto param = static_cast<const ast::MessageParam*>(decl->params[i]);
                out += (i == 0 ? "" : " ") + param->label + ": " + param->name + ":" + spell(message.params[i]->type, true);
            }
        }
        if (message.returnType != nullptr && message.returnType->kind != TypeKind::voidType) {
            out += " [" + spell(message.returnType, false) + "]";
        }
        return out;
    }

    void writeRecord(const RecordInfo& record)
    {
        out_ << "    " << (record.packed ? "@[packed] " : "") << "record " << record.name << " {";
        for (const FieldInfo& field : record.fields) {
            out_ << " " << field.name << ": " << spell(field.type, false) << ";";
        }
        out_ << " }\n";
    }

    void writeEnum(const EnumInfo& info)
    {
        out_ << "    enum " << info.name << " : " << info.storage->name << " {";
        for (const EnumMemberInfo& member : info.members) {
            ConstValue value;
            value.bits = member.value;
            out_ << " " << member.name << " := " << literal(value, info.storage) << ";";
        }
        out_ << " }\n";
    }

    void writeNamedType(const NamedTypeInfo& named)
    {
        const Type* type = named.type;
        if (type == nullptr) {
            return;
        }
        if (named.isAlias) {
            out_ << "    @[alias] type " << named.name << " := " << spell(type, false) << ";\n";
        }
        else if (isSubrange(type) && !isEnum(type->inner)) {
            out_ << "    type " << named.name << " : " << type->inner->name << " := " << type->low << " to: " << type->high << ";\n";
        }
        else if (isSubrange(type)) {
            const EnumInfo* info = type->inner->enumInfo;
            std::string enumName = spell(type->inner, false);
            out_ << "    type " << named.name << " := " << enumName << " " << info->members[static_cast<size_t>(type->low)].name << " to: " << enumName
                 << " " << info->members[static_cast<size_t>(type->high)].name << ";\n";
        }
        else {
            out_ << "    type " << named.name << " := " << spell(type->distinctOf, false) << ";\n";
        }
    }

    void writeClass(const ClassInfo& info)
    {
        out_ << "    " << (info.isProtocol ? "@[protocol] " : "") << "class " << info.name;
        if (info.superclass != nullptr) {
            out_ << " inherits " << spell(info.superclass->type, false);
        }
        if (info.isProtocol) {
            for (const ClassInfo* refined : info.protocols) {
                out_ << " inherits " << spell(refined->type, false);
            }
        }
        else if (!info.protocols.empty()) {
            out_ << " implements ";
            for (size_t i = 0; i < info.protocols.size(); ++i) {
                out_ << (i == 0 ? "" : ", ") << spell(info.protocols[i]->type, false);
            }
        }
        out_ << " {\n";
        // Own fields in index order (the layout), grouped by visibility.
        int group = -1;
        for (const FieldInfo& field : info.fields) {
            if (field.owner != &info) {
                continue;
            }
            int visibility = field.isPublic ? 1 : 0;
            if (visibility != group) {
                if (group != -1) {
                    out_ << " }\n";
                }
                out_ << "        " << (field.isPublic ? "public" : "private") << " {";
                group = visibility;
            }
            out_ << " " << (field.initializer != nullptr ? "@[initialized] " : "") << field.name << ": " << spell(field.type, false) << ";";
        }
        if (group != -1) {
            out_ << " }\n";
        }
        for (const MessageInfo* message : info.methodList) {
            if (message->owner == &info && !message->isCopy) {
                out_ << "        " << signature(*message, false) << ";\n";
            }
        }
        for (const auto& message : model_.messages) {
            if (message->owner == &info && !message->isInstance && message->decl != nullptr) {
                out_ << "        " << signature(*message, false) << ";\n";
            }
        }
        if (info.initMessage != nullptr) {
            out_ << "        " << signature(*info.initMessage, false) << ";\n";
        }
        out_ << "    }\n";
    }

    void writeConstant(const ConstantInfo& constant)
    {
        if (constant.item == nullptr || constant.item->value == nullptr) {
            return;
        }
        out_ << "    @[constant] " << constant.name << ": " << spell(constant.type, false) << " := " << literal(*constant.item->value, constant.type)
             << ";\n";
    }

    const ast::Unit* unit_;
    const SemanticModel& model_;
    std::ostringstream out_;
};

std::string hash(const std::string& text)
{
    uint64_t value = 14695981039346656037ull;
    for (unsigned char c : text) {
        value = (value ^ c) * 1099511628211ull;
    }
    char hex[20];
    std::snprintf(hex, sizeof(hex), "%016" PRIx64, value);
    return hex;
}

}

std::string writeInterface(const ast::Unit* unit, const SemanticModel& model, std::string& fingerprint)
{
    InterfaceWriter writer(unit, model);
    std::string body = writer.body();
    fingerprint = hash(body);
    std::string depends;
    for (const auto& module : model.modules) {
        depends += (depends.empty() ? "" : ";") + module->name + "=" + module->fingerprint;
    }
    std::string header = "# The interface of " + std::string(unit->unitKind == ast::UnitKind::library ? "lib" : "module") + " '" + unit->name +
                         "', written by bobc. Don't edit it: recompile " + unit->name + " instead.\n";
    header += "@[interface: \"" + fingerprint + "\"" + (depends.empty() ? "" : ", depends: \"" + depends + "\"") + "] " +
              (unit->unitKind == ast::UnitKind::library ? "lib " : "module ") + unit->name + ", " +
              (unit->version.empty() ? "0.0.0" : unit->version) + "\n";
    return header + body;
}

}
