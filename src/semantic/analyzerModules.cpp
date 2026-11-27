// Modules and libraries (design §11.5): reading imported interfaces, unit
// sections, name clashes between imports, and what an interface may expose.

#include "analyzerInternal.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <fstream>
#include <sstream>

namespace bob::sema::detail {

namespace {

std::vector<int> versionParts(const std::string& version)
{
    std::vector<int> parts;
    std::stringstream stream(version);
    std::string part;
    while (std::getline(stream, part, '.')) {
        parts.push_back(part.empty() ? 0 : std::atoi(part.c_str()));
    }
    while (parts.size() < 3) {
        parts.push_back(0);
    }
    return parts;
}

// `import x, 1.4;` accepts 1.4.0 and later with the same major version.
bool versionSatisfies(const std::string& available, const std::string& requested)
{
    if (requested.empty()) {
        return true;
    }
    std::vector<int> have = versionParts(available);
    std::vector<int> want = versionParts(requested);
    if (have[0] != want[0]) {
        return false;
    }
    return std::make_pair(have[1], have[2]) >= std::make_pair(want[1], want[2]);
}

const Attribute* attributeNamed(const Node* node, std::string_view name)
{
    for (const Attribute* attribute : node->attributes) {
        if (attribute->name == name) {
            return attribute;
        }
    }
    return nullptr;
}

std::string stringAttribute(const Node* node, std::string_view name)
{
    const Attribute* attribute = attributeNamed(node, name);
    auto value = attribute != nullptr ? nodeCast<StringLiteral>(attribute->value) : nullptr;
    return value != nullptr ? value->value : "";
}

}

// ---------------------------------------------------------------------------
// loading

void Analyzer::loadImports(const std::vector<Import*>& imports, const std::string& directory, const ModuleInfo* importer)
{
    for (const Import* import : imports) {
        loadModule(import, directory, importer);
    }
}

ModuleInfo* Analyzer::loadModule(const Import* import, const std::string& directory, const ModuleInfo* importer)
{
    std::string importerName = importer != nullptr ? "'" + importer->name + "'" : "this unit";
    if (ModuleInfo* loaded = model_.moduleNamed(import->name)) {
        if (!versionSatisfies(loaded->version, import->version)) {
            error(import->nameRange, "sema.importVersion",
                  importerName + " needs '" + import->name + "' " + import->version + ", but version " + loaded->version + " is imported");
        }
        return loaded;
    }
    if (import->name == model_.program.name) {
        error(import->nameRange, "sema.importSelf", "a unit can't import itself");
        return nullptr;
    }

    // `core.io` -> core.io.bobi, in the importer's directory first, then the module paths.
    std::vector<std::string> directories;
    if (!directory.empty()) {
        directories.push_back(directory);
    }
    directories.insert(directories.end(), options_.modulePaths.begin(), options_.modulePaths.end());
    std::filesystem::path found;
    for (const std::string& dir : directories) {
        std::filesystem::path candidate = std::filesystem::path(dir) / (import->name + ".bobi");
        if (std::filesystem::exists(candidate)) {
            found = candidate;
            break;
        }
    }
    if (found.empty()) {
        std::string searched;
        for (const std::string& dir : directories) {
            searched += (searched.empty() ? "" : ", ") + dir;
        }
        error(import->nameRange, "sema.importNotFound",
              "can't find module '" + import->name + "': no " + import->name + ".bobi in " + (searched.empty() ? "the module paths" : searched) +
                  " (compile it with 'bobc', or add its directory with --module-path=)");
        return nullptr;
    }

    auto module = std::make_unique<ModuleInfo>();
    module->name = import->name;
    module->interfacePath = found.string();
    {
        std::ifstream in(found, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        module->text = text.str();
    }
    DiagnosticEngine parseDiagnostics;
    // Node ids key the whole model, so each interface gets its own range
    // (the unit being compiled uses ids below 2^24).
    ast::NodeId firstId = static_cast<ast::NodeId>((model_.modules.size() + 1) << 24);
    module->parsed = std::make_unique<parser::ParsedUnit>(parser::parseSource(module->text, parseDiagnostics, firstId));
    const Unit* unit = module->parsed->unit;
    if (parseDiagnostics.errorCount() > 0 || unit == nullptr ||
        (unit->unitKind != UnitKind::module && unit->unitKind != UnitKind::library) || unit->name != import->name ||
        attributeNamed(unit, "interface") == nullptr) {
        error(import->nameRange, "sema.badInterface",
              "'" + module->interfacePath + "' isn't a valid interface of '" + import->name + "'; recompile '" + import->name + "'");
        return nullptr;
    }
    module->version = unit->version;
    module->fingerprint = stringAttribute(unit, "interface");
    std::stringstream depends(stringAttribute(unit, "depends"));
    std::string entry;
    while (std::getline(depends, entry, ';')) {
        size_t equals = entry.find('=');
        if (equals != std::string::npos) {
            module->dependencies.push_back({entry.substr(0, equals), entry.substr(equals + 1)});
        }
    }
    for (const char* extension : {".lib", ".obj"}) {
        std::filesystem::path library = found;
        library.replace_extension(extension);
        if (std::filesystem::exists(library)) {
            module->libraryPath = library.string();
            break;
        }
    }
    if (!versionSatisfies(module->version, import->version)) {
        error(import->nameRange, "sema.importVersion",
              importerName + " needs '" + import->name + "' " + import->version + ", but '" + module->interfacePath + "' is version " +
                  module->version);
    }
    module->program.name = module->name;
    module->type = types_.addProgram(&module->program);
    ModuleInfo* raw = module.get();
    model_.modules.push_back(std::move(module));

    // Its own imports first (their declarations appear in its interface), with
    // the Pascal check: it must have been compiled against the interfaces we load.
    loadImports(unit->imports, found.parent_path().string(), raw);
    for (const auto& [name, fingerprint] : raw->dependencies) {
        const ModuleInfo* dependency = model_.moduleNamed(name);
        if (dependency != nullptr && dependency->fingerprint != fingerprint) {
            error(import->nameRange, "sema.staleModule",
                  "'" + raw->name + "' was compiled against a different interface of '" + name + "' (" + dependency->interfacePath +
                      "); recompile '" + raw->name + "'");
        }
    }

    ModuleInfo* savedModule = unitModule_;
    ProgramInfo* savedProgram = unitProgram_;
    bool savedExporting = exporting_;
    unitModule_ = raw;
    unitProgram_ = &raw->program;
    exporting_ = false;
    collect(unit);
    unitModule_ = savedModule;
    unitProgram_ = savedProgram;
    exporting_ = savedExporting;
    return raw;
}

// ---------------------------------------------------------------------------
// sections, names

std::vector<std::pair<const Node*, bool>> Analyzer::unitItems(const Unit* unit)
{
    std::vector<std::pair<const Node*, bool>> items;
    bool sectionsAllowed = unit->unitKind == UnitKind::module || unit->unitKind == UnitKind::library;
    for (const Node* item : unit->items) {
        auto section = nodeCast<UnitSection>(item);
        if (section == nullptr) {
            items.push_back({item, false});
            continue;
        }
        if (!sectionsAllowed && !importing()) {
            error(section->range.begin == section->range.end ? section->range : SourceRange{section->range.begin, section->range.begin + 9},
                  "sema.sectionInProgram",
                  "only 'module' and 'lib' units have an interface; a program's declarations are all its own, so it has no sections");
        }
        for (const Node* inner : section->items) {
            if (inner->kind == NodeKind::unitSection) {
                error(inner, "sema.nestedSection", "sections can't be nested");
                continue;
            }
            items.push_back({inner, section->isInterface});
        }
    }
    return items;
}

bool Analyzer::declaredHere(const std::string& name) const
{
    auto here = [&](const Origin& origin) { return origin.module == unitModule_; };
    for (const auto& r : model_.records) {
        if (r->name == name && here(r->origin)) {
            return true;
        }
    }
    for (const auto& c : model_.classes) {
        if (c->name == name && here(c->origin)) {
            return true;
        }
    }
    for (const auto& e : model_.enums) {
        if (e->name == name && here(e->origin)) {
            return true;
        }
    }
    for (const auto& n : model_.namedTypes) {
        if (n->name == name && here(n->origin)) {
            return true;
        }
    }
    for (const auto& k : model_.constants) {
        if (k->name == name && here(k->origin)) {
            return true;
        }
    }
    return types_.builtin(name) != nullptr || name == unitProgram_->name;
}

// An unqualified name the unit doesn't declare itself, declared by two imports.
void Analyzer::checkUnambiguous(const std::string& name, SourceRange range)
{
    if (name.find('.') != std::string::npos || model_.modules.size() < 2) {
        return;
    }
    std::vector<std::string> modules;
    auto consider = [&](const std::string& itemName, const Origin& origin) {
        if (itemName != name) {
            return false;
        }
        if (origin.module == nullptr) {
            return true;
        }
        if (std::find(modules.begin(), modules.end(), origin.module->name) == modules.end()) {
            modules.push_back(origin.module->name);
        }
        return false;
    };
    bool own = false;
    for (const auto& r : model_.records) {
        own = consider(r->name, r->origin) || own;
    }
    for (const auto& c : model_.classes) {
        own = consider(c->name, c->origin) || own;
    }
    for (const auto& e : model_.enums) {
        own = consider(e->name, e->origin) || own;
    }
    for (const auto& n : model_.namedTypes) {
        own = consider(n->name, n->origin) || own;
    }
    for (const auto& k : model_.constants) {
        own = consider(k->name, k->origin) || own;
    }
    if (!own && modules.size() > 1 && !importing()) {
        error(range, "sema.ambiguousName",
              "'" + name + "' is declared in both '" + modules[0] + "' and '" + modules[1] + "'; write '" + modules[0] + "." + name + "' or '" +
                  modules[1] + "." + name + "'");
    }
}

void Analyzer::applyInterfaceAttributes(MessageInfo* message, const MessageDecl* decl)
{
    message->imported = true;
    message->paramEscapes.assign(message->params.size(), false);
    std::stringstream keeps(stringAttribute(decl, "keeps"));
    std::string name;
    while (keeps >> name) {
        if (name == "self") {
            message->selfEscapes = true;
        }
        for (size_t i = 0; i < message->params.size(); ++i) {
            if (message->params[i]->name == name) {
                message->paramEscapes[i] = true;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// exports

void Analyzer::checkExports(const Unit* unit)
{
    if (unit->unitKind != UnitKind::module && unit->unitKind != UnitKind::library) {
        return;
    }
    // A type an interface can mention: built in, imported, or declared in the
    // interface section; containers of those.
    std::function<bool(const Type*)> visible = [&](const Type* type) -> bool {
        type = canonical(type);
        if (type == nullptr || type->kind == TypeKind::error) {
            return true;
        }
        switch (type->kind) {
        case TypeKind::record:
            return type->record->origin.module != nullptr || type->record->origin.exported;
        case TypeKind::classType:
            return type->classInfo->origin.module != nullptr || type->classInfo->origin.exported || type->classInfo == model_.fieldMetaClass;
        case TypeKind::enumType:
            return type->enumInfo->origin.module != nullptr || type->enumInfo->origin.exported;
        case TypeKind::stringView:
        case TypeKind::metaInfo:
            return false;
        case TypeKind::block:
            for (const Type* param : type->params) {
                if (!visible(param)) {
                    return false;
                }
            }
            return visible(type->result);
        default:
            break;
        }
        if (type->distinctOf != nullptr || isSubrange(type)) {
            for (const auto& named : model_.namedTypes) {
                if (named->type == type) {
                    return named->origin.module != nullptr || named->origin.exported;
                }
            }
            if (isSubrange(type) && !visible(type->inner)) {
                return false;
            }
        }
        return (type->inner == nullptr || visible(type->inner)) && (type->key == nullptr || visible(type->key));
    };
    auto require = [&](const Type* type, SourceRange range, const std::string& what) {
        if (!visible(type)) {
            error(range, "sema.privateInInterface",
                  what + " uses '" + typeName(type) + "', which isn't in the interface section; move it there or keep this declaration in 'implementation'");
        }
    };
    auto checkMessage = [&](const MessageInfo* message, const std::string& owner) {
        if (message == nullptr || message->decl == nullptr) {
            return;
        }
        for (const LocalInfo* param : message->params) {
            require(param->type, message->decl->selectorRange, owner + "'" + message->selector + "'");
        }
        require(message->returnType, message->decl->selectorRange, owner + "'" + message->selector + "'");
    };
    for (const auto& record : model_.records) {
        if (record->origin.exported) {
            for (const FieldInfo& field : record->fields) {
                require(field.type, field.decl != nullptr ? field.decl->nameRange : record->decl->nameRange, "the field '" + field.name + "'");
            }
        }
    }
    for (const auto& info : model_.classes) {
        if (!info->origin.exported) {
            continue;
        }
        if (info->superclass != nullptr) {
            require(info->superclass->type, info->decl->nameRange, "'" + info->name + "'");
        }
        for (const ClassInfo* protocol : info->protocols) {
            require(protocol->type, info->decl->nameRange, "'" + info->name + "'");
        }
        for (const FieldInfo& field : info->fields) {
            if (field.owner == info.get()) {
                // Importers need the whole layout, private fields included.
                require(field.type, field.decl->nameRange, "the field '" + field.name + "'");
            }
        }
        for (const auto& message : model_.messages) {
            if (message->owner == info.get() && !message->isCopy && message.get() != info->initializer) {
                checkMessage(message.get(), "'" + info->name + " ");
            }
        }
    }
    for (const auto& named : model_.namedTypes) {
        if (named->origin.exported && named->type != nullptr) {
            const Type* base = isSubrange(named->type) ? named->type->inner : named->type->distinctOf;
            if (base != nullptr) {
                require(base, named->decl->nameRange, "the type '" + named->name + "'");
            }
        }
    }
    for (const auto& constant : model_.constants) {
        if (!constant->origin.exported || constant->type == nullptr) {
            continue;
        }
        require(constant->type, constant->nameRange, "the constant '" + constant->name + "'");
        std::function<bool(const Type*)> writable = [&](const Type* type) -> bool {
            type = canonical(type);
            if (type->kind == TypeKind::dictionary || type->kind == TypeKind::hashtable) {
                return false;
            }
            return type->inner == nullptr || writable(type->inner);
        };
        if (!writable(constant->type)) {
            error(constant->nameRange, "sema.privateInInterface",
                  "an exported constant can't hold a dictionary or hashtable yet; keep '" + constant->name + "' in 'implementation'");
        }
    }
    for (const MessageInfo* message : model_.program.messages) {
        if (message->origin.exported) {
            checkMessage(message, "'" + model_.program.name + " ");
        }
    }
}

}
