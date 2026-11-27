#include "syntaxTestSupport.h"

#include <gtest/gtest.h>

using namespace bob::ast;
using bobTest::parse;

TEST(Ast, ParentsAreLinked)
{
    auto parsed = parse("{ class C { msg m { x := 1 + 2; } } }");
    Unit* unit = parsed->unit.unit;
    std::vector<Node*> pending{unit};
    size_t visited = 0;
    while (!pending.empty()) {
        Node* node = pending.back();
        pending.pop_back();
        ++visited;
        for (const ChildRef& child : childrenOf(node)) {
            EXPECT_EQ(child.node->parent, node) << nodeKindName(child.node->kind);
            pending.push_back(child.node);
        }
    }
    EXPECT_GT(visited, 8u);
    EXPECT_EQ(unit->parent, nullptr);
}

TEST(Ast, IdsResolveThroughTheArena)
{
    auto parsed = parse("{ a := 1; }");
    Node* item = parsed->unit.unit->items.front();
    EXPECT_EQ(parsed->unit.arena->byId(item->id), item);
    EXPECT_EQ(parsed->unit.arena->byId(0), nullptr);
}

TEST(Ast, ChildRangesNestInsideParents)
{
    auto parsed = parse("program p, 1.0.0 {\n@[protocol] class S { msg area [real64]; }\nrecord R { x: uint8; }\nv := (a + b) * c foo: [1, 2];\n}");
    ASSERT_EQ(parsed->diagnostics.errorCount(), 0u) << parsed->diagnosticsText();
    std::vector<Node*> pending{parsed->unit.unit};
    while (!pending.empty()) {
        Node* node = pending.back();
        pending.pop_back();
        for (const ChildRef& child : childrenOf(node)) {
            EXPECT_GE(child.node->range.begin, node->range.begin) << nodeKindName(child.node->kind);
            EXPECT_LE(child.node->range.end, node->range.end) << nodeKindName(child.node->kind);
            pending.push_back(child.node);
        }
    }
}

TEST(Ast, FindInnermostNode)
{
    std::string source = "{ total := count + 42; }";
    auto parsed = parse(source);
    uint32_t offset = static_cast<uint32_t>(source.find("42"));
    Node* node = findInnermostNode(parsed->unit.unit, offset);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, NodeKind::integerLiteral);
    EXPECT_EQ(node->parent->kind, NodeKind::binarySend);

    uint32_t countOffset = static_cast<uint32_t>(source.find("count"));
    Node* identifier = findInnermostNode(parsed->unit.unit, countOffset);
    ASSERT_NE(nodeCast<Identifier>(identifier), nullptr);
    EXPECT_EQ(nodeCast<Identifier>(identifier)->name, "count");
}

TEST(Ast, NameRangesPointAtTheName)
{
    std::string source = "{ class Widget { msg resize: w:int32 by: h:int32 { } } }";
    auto parsed = parse(source);
    auto classDecl = nodeCast<ClassDecl>(parsed->unit.unit->items.front());
    ASSERT_NE(classDecl, nullptr);
    EXPECT_EQ(source.substr(classDecl->nameRange.begin, classDecl->nameRange.end - classDecl->nameRange.begin), "Widget");
    auto message = nodeCast<MessageDecl>(classDecl->members.front());
    ASSERT_NE(message, nullptr);
    auto second = nodeCast<MessageParam>(message->params[1]);
    EXPECT_EQ(source.substr(second->labelRange.begin, second->labelRange.end - second->labelRange.begin), "by");
    EXPECT_EQ(source.substr(second->nameRange.begin, second->nameRange.end - second->nameRange.begin), "h");
}

TEST(Ast, JsonDumpHasStableShape)
{
    std::string source = "{ x := 'é'; }";
    auto parsed = parse(source);
    bob::SourceFile file("t.bob", source);
    std::string json = dumpAstJson(parsed->unit.unit, &file);
    EXPECT_NE(json.find("\"kind\": \"Unit\""), std::string::npos) << json;
    EXPECT_NE(json.find("\"role\": \"item\""), std::string::npos) << json;
    EXPECT_NE(json.find("\"codepoint\": \"U+00E9\""), std::string::npos) << json;
    EXPECT_NE(json.find("\"line\": 1, \"column\": 3"), std::string::npos) << json;
}
