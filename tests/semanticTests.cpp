#include "analyzer.h"
#include "parser/parser.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace {

struct Analyzed {
    std::string source;
    bob::DiagnosticEngine diagnostics;
    bob::parser::ParsedUnit parsed;
    bob::sema::SemanticModel model;

    bool hasCode(const std::string& code) const
    {
        for (const auto& diagnostic : diagnostics.diagnostics()) {
            if (diagnostic.code == code) {
                return true;
            }
        }
        return false;
    }

    std::string text() const
    {
        bob::SourceFile file("test.bob", source);
        return diagnostics.format(file);
    }
};

// Wraps `body` in a program with a `main` and analyzes it.
std::unique_ptr<Analyzed> analyzeMain(const std::string& body, const std::string& extraMessages = "", const std::string& items = "",
                                      const bob::sema::AnalyzeOptions& options = {}, const std::string& header = "")
{
    auto result = std::make_unique<Analyzed>();
    result->source = header + "program p, 1.0.0\n{\n" + items + "\nextends p {\n" + extraMessages +
                     "\n@[static] msg main: args:array [int32] {\n" + body + "\n}\n}\n}\n";
    result->parsed = bob::parser::parseSource(result->source, result->diagnostics);
    EXPECT_EQ(result->diagnostics.errorCount(), 0u) << result->text();
    bob::sema::analyze(result->parsed.unit, result->model, result->diagnostics, options);
    return result;
}

const bob::sema::LocalInfo* localNamed(const Analyzed& analyzed, const std::string& name)
{
    for (const auto& local : analyzed.model.locals) {
        if (local->name == name) {
            return local.get();
        }
    }
    return nullptr;
}

}

TEST(Semantic, CleanProgramRecordsTypes)
{
    auto a = analyzeMain("x := 4;\ny: int64 := 5;\nz := y + x;\nr := 2.5;\nok := x < 3;\nreturn 0;");
    EXPECT_EQ(a->diagnostics.diagnostics().size(), 0u) << a->text();
    EXPECT_EQ(localNamed(*a, "x")->type->name, "int32");
    EXPECT_EQ(localNamed(*a, "y")->type->name, "int64");
    EXPECT_EQ(localNamed(*a, "z")->type->name, "int64");
    EXPECT_EQ(localNamed(*a, "r")->type->name, "real64");
    EXPECT_EQ(localNamed(*a, "ok")->type->name, "bool");
    EXPECT_EQ(a->model.implicitDeclarations.size(), 4u);
}

TEST(Semantic, LiteralTakesTypeFromOtherOperand)
{
    auto a = analyzeMain("big: int64 := 1;\nv := 3000000000 + big;\nreturn 0;");
    EXPECT_EQ(a->diagnostics.diagnostics().size(), 0u) << a->text();
    EXPECT_EQ(localNamed(*a, "v")->type->name, "int64");
}

TEST(Semantic, AndOrXorAreLogicalOrBitwiseByOperands)
{
    auto a = analyzeMain("x := 6;\nm := x and 3;\nk := 5 xor x;\nb := true xor false;\nc := x > 1 and x < 9;\nbad := x and 3 = 0;\nreturn 0;");
    EXPECT_EQ(localNamed(*a, "m")->type->name, "int32");
    EXPECT_EQ(localNamed(*a, "k")->type->name, "int32");
    EXPECT_EQ(localNamed(*a, "b")->type->name, "bool");
    EXPECT_EQ(localNamed(*a, "c")->type->name, "bool");
    EXPECT_TRUE(a->hasCode("sema.typeMismatch")) << a->text(); // `x and (3 = 0)`: parenthesise the mask
    EXPECT_EQ(a->diagnostics.errorCount(), 1u) << a->text();
}

TEST(Semantic, ImplicitDeclarationCantReadItself)
{
    auto a = analyzeMain("count := count + 1;\nreturn 0;");
    EXPECT_TRUE(a->hasCode("sema.undeclared")) << a->text();
}

TEST(Semantic, NarrowingNeedsExplicitConversion)
{
    auto a = analyzeMain("y := 5;\nx: int8 := y;\nreturn 0;");
    EXPECT_TRUE(a->hasCode("sema.typeMismatch")) << a->text();
    EXPECT_NE(a->text().find("asInt8"), std::string::npos) << a->text();
}

TEST(Semantic, LiteralOutOfRange)
{
    auto a = analyzeMain("x: int8 := 300;\nu: uint8 := -1;\nreturn 0;");
    EXPECT_TRUE(a->hasCode("sema.literalOutOfRange")) << a->text();
    EXPECT_EQ(a->diagnostics.errorCount(), 2u) << a->text();
}

TEST(Semantic, MismatchedSignedness)
{
    auto a = analyzeMain("a: int64 := 1;\nb: uint64 := 2;\nc := a + b;\nreturn 0;");
    EXPECT_TRUE(a->hasCode("sema.typeMismatch")) << a->text();
}

TEST(Semantic, ConditionMustBeBool)
{
    auto a = analyzeMain("if 1 {\n}\nreturn 0;");
    EXPECT_TRUE(a->hasCode("sema.conditionNotBool")) << a->text();
}

TEST(Semantic, BreakOutsideLoop)
{
    auto a = analyzeMain("break;\nreturn 0;");
    EXPECT_TRUE(a->hasCode("sema.jumpOutsideLoop")) << a->text();
}

TEST(Semantic, MissingReturn)
{
    auto a = analyzeMain("x := 1;\nif x > 0 {\n return 1;\n}");
    EXPECT_TRUE(a->hasCode("sema.missingReturn")) << a->text();
    auto ok = analyzeMain("loop {\n}\n");
    EXPECT_FALSE(ok->hasCode("sema.missingReturn")) << ok->text();
}

TEST(Semantic, ParametersAndLoopVariablesAreReadOnly)
{
    auto a = analyzeMain("for i in 1 to: 3 {\n i := 2;\n}\nreturn 0;", "@[static] msg f: n:int32 { n := 1; }");
    EXPECT_TRUE(a->hasCode("sema.assignToReadOnly")) << a->text();
    EXPECT_EQ(a->diagnostics.errorCount(), 2u) << a->text();
}

TEST(Semantic, RedeclarationInSameScope)
{
    auto a = analyzeMain("x: int32;\nx: int64;\nreturn 0;");
    EXPECT_TRUE(a->hasCode("sema.redeclared")) << a->text();
}

TEST(Semantic, ShadowingInInnerScopeIsAllowedExplicitly)
{
    auto a = analyzeMain("x := 1;\nif x > 0 {\n x: int64 := 2;\n}\nreturn x;");
    EXPECT_EQ(a->diagnostics.errorCount(), 0u) << a->text();
}

TEST(Semantic, StaticMessageCallsAreResolved)
{
    auto a = analyzeMain("return self twice: 21;", "@[static] msg twice: n:int32 [int32] { return n * 2; }");
    EXPECT_EQ(a->diagnostics.errorCount(), 0u) << a->text();
    bool found = false;
    for (const auto& [id, send] : a->model.sends) {
        if (send.kind == bob::sema::SendKind::staticCall && send.target != nullptr && send.target->selector == "twice:") {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(Semantic, UnknownMessageAndField)
{
    auto a = analyzeMain("pt := Point x: 1 y: 2;\nz := pt.z;\nq := self nope;\nreturn 0;", "", "record Point { x: int32; y: int32; }");
    EXPECT_TRUE(a->hasCode("sema.unknownField")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.unknownMessage")) << a->text();
}

TEST(Semantic, RecordConstructorNeedsEveryFieldInOrder)
{
    auto a = analyzeMain("p := Point y: 1 x: 2;\nreturn 0;", "", "record Point { x: int32; y: int32; }");
    EXPECT_TRUE(a->hasCode("sema.unknownMessage")) << a->text();
    EXPECT_NE(a->text().find("Point x:y:"), std::string::npos) << a->text();
}

TEST(Semantic, RecursiveRecordIsRejected)
{
    auto a = analyzeMain("return 0;", "", "record A { b: B; }\nrecord B { a: A; }");
    EXPECT_TRUE(a->hasCode("sema.recursiveRecord")) << a->text();
}

TEST(Semantic, CaseNeedsElseAndDistinctLabels)
{
    auto a = analyzeMain("x := 3;\ncase x of {\n 1: { }\n (0 to: 2): { }\n}\nreturn 0;");
    EXPECT_TRUE(a->hasCode("sema.caseNotExhaustive")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.overlappingCaseLabels")) << a->text();
}

TEST(Semantic, MainIsRequiredAndMessagesAreStatic)
{
    auto result = std::make_unique<Analyzed>();
    result->source = "program p, 1.0.0 { extends p { msg helper { } } }";
    result->parsed = bob::parser::parseSource(result->source, result->diagnostics);
    bob::sema::analyze(result->parsed.unit, result->model, result->diagnostics);
    EXPECT_TRUE(result->hasCode("sema.noMain")) << result->text();
    EXPECT_TRUE(result->hasCode("sema.programMessageNotStatic")) << result->text();
}

TEST(Semantic, UnsupportedFeaturesNameTheirPhase)
{
    auto a = analyzeMain("p: pointer;\nreturn 0;", "", "class Widget { }\nnamespace n { }");
    EXPECT_TRUE(a->hasCode("sema.unsupported")) << a->text();
    std::string text = a->text();
    EXPECT_NE(text.find("the type 'pointer' isn't supported yet (planned for phase 9)"), std::string::npos) << text;
    EXPECT_NE(text.find("namespaces isn't supported yet (planned for phase 7)"), std::string::npos) << text;
}

TEST(Semantic, BareBlockParameterIsInferredFromTheBody)
{
    auto a = analyzeMain("return self apply: { [n] n * 2 } to: 21;", "@[static] msg apply: f:block to: x:int32 [int32] { return f value: x; }");
    EXPECT_EQ(a->diagnostics.errorCount(), 0u) << a->text();
    const bob::sema::LocalInfo* f = localNamed(*a, "f");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->type->name, "{ int32 [int32] }");
    const bob::sema::LocalInfo* n = localNamed(*a, "n");
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->type->name, "int32");
}

TEST(Semantic, UnusedBareBlockParameterNeedsAnAnnotation)
{
    auto a = analyzeMain("return 0;", "@[static] msg run: f:block { }");
    EXPECT_TRUE(a->hasCode("sema.cannotInferBlock")) << a->text();
}

TEST(Semantic, BlockParameterTypeNeedsContextOrAnnotation)
{
    auto a = analyzeMain("sq := { [i] i * i };\nreturn 0;");
    EXPECT_TRUE(a->hasCode("sema.cannotInferBlock")) << a->text();
}

TEST(Semantic, ReturnOnlyInsideInlinedBlocks)
{
    auto a = analyzeMain("b := { return 1; };\nx := 1;\n(x > 0) ifTrue: { return 2; };\nreturn 0;");
    EXPECT_TRUE(a->hasCode("sema.returnInBlock")) << a->text();
    EXPECT_EQ(a->diagnostics.errorCount(), 1u) << a->text();
}

TEST(Semantic, BreakDoesNotCrossABlockFunction)
{
    auto a = analyzeMain("loop {\n b := { break; };\n break;\n}\nreturn 0;");
    EXPECT_TRUE(a->hasCode("sema.jumpOutsideLoop")) << a->text();
    EXPECT_EQ(a->diagnostics.errorCount(), 1u) << a->text();
}

TEST(Semantic, BlockTypesInFieldsAndResults)
{
    auto a = analyzeMain("e: { } := { };\nreturn 0;",
                         "@[static] msg make [{ int32 [int32] }] { return { [x] x }; }\n"
                         "@[static] msg bare [block] { return { 1 }; }\n"
                         "@[static] msg old: f:[block takes: [int32]] { }",
                         "record Holder { f: { [int32] }; }\n"
                         "class Button { public { onPress: { [int32] }; onHover: block?; } }\n"
                         "class Hidden { private { run: { [int32] }; } }\n"
                         "class Bare { public { f: block; } }");
    EXPECT_TRUE(a->hasCode("sema.referenceInRecord")) << a->text();
    EXPECT_NE(a->text().find("a bare 'block' can't be the type of a field"), std::string::npos) << a->text();
    EXPECT_NE(a->text().find("a bare 'block' can't be a message's return type"), std::string::npos) << a->text();
    EXPECT_TRUE(a->hasCode("sema.invalidOptional")) << a->text(); // a bare `block?` can't be inferred
    EXPECT_NE(a->text().find("private field 'run' needs an initializer"), std::string::npos) << a->text();
    EXPECT_NE(a->text().find("block types are written in braces"), std::string::npos) << a->text();
    EXPECT_EQ(a->diagnostics.errorCount(), 6u) << a->text();
    EXPECT_EQ(localNamed(*a, "e")->type->name, "{ }");
}

namespace {

// The block function that captures `name`.
const bob::sema::BlockInfo* blockCapturing(const Analyzed& analyzed, const std::string& name)
{
    for (const auto& info : analyzed.model.blockInfos) {
        for (const bob::sema::LocalInfo* captured : info->captures) {
            if (!info->inlined && captured->name == name) {
                return info.get();
            }
        }
    }
    return nullptr;
}

bool isStackBlock(const Analyzed& analyzed, const std::string& captured)
{
    return analyzed.model.stackBlocks.count(blockCapturing(analyzed, captured)) != 0;
}

bool isBoxed(const Analyzed& analyzed, const std::string& name)
{
    return analyzed.model.boxedLocals.count(localNamed(analyzed, name)) != 0;
}

}

TEST(Semantic, NamedBlockTypesNameTheShape)
{
    auto a = analyzeMain("f: Transform := { [x: int32] x * 2 };\ng: { int32 [int32] } := f;\nh: Handler := nil;\nreturn f value: 1;", "",
                         "type Transform := { int32 [int32] };\n"
                         "type Handler := { [int32] }?;\n"
                         "type Bare := block;");
    EXPECT_NE(a->text().find("a bare 'block' can't be a named type"), std::string::npos) << a->text();
    EXPECT_EQ(a->diagnostics.errorCount(), 1u) << a->text(); // Transform and its shape are the same type
    EXPECT_TRUE(bob::sema::sameBlockShape(localNamed(*a, "f")->type, localNamed(*a, "g")->type));
    EXPECT_EQ(localNamed(*a, "f")->type->name, "{ int32 [int32] }");
    EXPECT_TRUE(a->model.namedType("Transform")->isAlias);
}

TEST(Semantic, EscapingBlocksBecomeHeapClosures)
{
    auto a = analyzeMain("k := 1;\nf := { k + 1 };\nf value;\n"
                         "j := 2;\ng := self keep: { j };\n"
                         "q := 3;\nh: { [int32] }?;\nloop { h := { q }; break; }\n"
                         "return 0;",
                         "@[static] msg make [{ [int32] }] { n := 0; return { n := n + 1; n }; }\n"
                         "@[static] msg keep: f:{ [int32] } [{ [int32] }] { return f; }\n"
                         "@[static] msg adder: p:int32 [{ int32 [int32] }] { return { [x: int32] x + p }; }");
    EXPECT_EQ(a->diagnostics.errorCount(), 0u) << a->text();
    EXPECT_TRUE(isStackBlock(*a, "k"));
    EXPECT_FALSE(isBoxed(*a, "k"));
    EXPECT_FALSE(isStackBlock(*a, "n")); // returned
    EXPECT_TRUE(isBoxed(*a, "n"));
    EXPECT_FALSE(isStackBlock(*a, "j")); // passed to a message that keeps it
    EXPECT_TRUE(isBoxed(*a, "j"));
    EXPECT_FALSE(isStackBlock(*a, "q")); // assigned to a variable that outlives the loop
    EXPECT_FALSE(isStackBlock(*a, "p"));
    EXPECT_FALSE(isBoxed(*a, "p"));      // read-only: copied into the closure
    EXPECT_NE(a->model.blockEscapes.at(blockCapturing(*a, "n")).find("returned"), std::string::npos);
}

TEST(Semantic, WeakAndUnownedReferences)
{
    auto a = analyzeMain("n: Leaf;\n@[weak] me := n;\n@[weak] bad: Leaf := n;\n@[unowned] o: Leaf;\n@[weak] count := 3;\nme := n;\n"
                         "x := 1;\n@[weak] x := 2;\nreturn 0;",
                         "",
                         "class Leaf { public { size: int32; } }\n"
                         "class Node { public { @[weak] parent: Node?; @[unowned] owner: Node; @[weak] strongType: Node; } }");
    EXPECT_EQ(localNamed(*a, "me")->ownership, bob::sema::Ownership::weak);
    EXPECT_EQ(localNamed(*a, "me")->type->name, "Leaf?"); // inferred weak references become optional
    EXPECT_NE(a->text().find("its type must be optional: 'Leaf?'"), std::string::npos) << a->text();
    EXPECT_NE(a->text().find("its type must be optional: 'Node?'"), std::string::npos) << a->text();
    EXPECT_NE(a->text().find("'o' is @[unowned] and needs a value"), std::string::npos) << a->text();
    EXPECT_NE(a->text().find("only references to objects can be @[weak]"), std::string::npos) << a->text();
    EXPECT_NE(a->text().find("@[weak] goes on the declaration of 'x'"), std::string::npos) << a->text();
    EXPECT_EQ(a->diagnostics.errorCount(), 5u) << a->text();
    const bob::sema::ClassInfo* node = a->model.classNamed("Node");
    EXPECT_EQ(node->field("parent")->ownership, bob::sema::Ownership::weak);
    EXPECT_TRUE(node->field("owner")->required); // an unowned field must be passed
}

TEST(Semantic, CapturedObjectsLeaveTheStack)
{
    auto a = analyzeMain("local: Box;\nf := { local size };\nf value;\nreturn 0;",
                         "@[static] msg make [{ [int32] }] { kept: Box; return { kept size }; }",
                         "class Box { public { size: int32 := 4; } }");
    EXPECT_EQ(a->diagnostics.errorCount(), 0u) << a->text();
    bool localOnStack = false;
    bool keptOnStack = false;
    for (const auto& [id, object] : a->model.stackObjects) {
        localOnStack = localOnStack || object.local->name == "local";
        keptOnStack = keptOnStack || object.local->name == "kept";
    }
    EXPECT_TRUE(localOnStack);
    EXPECT_FALSE(keptOnStack);
}

TEST(Semantic, BlockArityMismatch)
{
    auto a = analyzeMain("return self apply: { [a, b] a } to: 1;", "@[static] msg apply: f:{ int32 [int32] } to: x:int32 [int32] { return f value: x; }");
    EXPECT_TRUE(a->hasCode("sema.blockArity")) << a->text();
}

TEST(Semantic, InlinedMessagesNeedBlockLiterals)
{
    auto a = analyzeMain("b := { 1 };\nx := 1;\n(x > 0) ifTrue: b;\n1 to: 3 do: { };\nreturn 0;");
    EXPECT_TRUE(a->hasCode("sema.inlinedNeedsBlock")) << a->text();
    EXPECT_EQ(a->diagnostics.errorCount(), 2u) << a->text();
}

TEST(Semantic, CapturesAreRecorded)
{
    auto a = analyzeMain("total := 0;\nlimit := 3;\nf := { [x: int32] total := total + x + limit; };\nf value: 1;\nreturn total;");
    EXPECT_EQ(a->diagnostics.errorCount(), 0u) << a->text();
    const bob::sema::BlockInfo* block = nullptr;
    for (const auto& info : a->model.blockInfos) {
        if (!info->inlined) {
            block = info.get();
        }
    }
    ASSERT_NE(block, nullptr);
    ASSERT_EQ(block->captures.size(), 2u);
    EXPECT_EQ(block->captures[0]->name, "total");
    EXPECT_EQ(block->captures[1]->name, "limit");
}

namespace {

bob::sema::SendKind kindOfSend(const Analyzed& a, const std::string& selector, const std::string& receiverClass)
{
    for (const auto& [id, send] : a.model.sends) {
        if (send.target != nullptr && send.target->selector == selector && send.classInfo != nullptr &&
            send.classInfo->name == receiverClass) {
            return send.kind;
        }
    }
    return bob::sema::SendKind::interval;
}

}

TEST(Semantic, DispatchTierFollowsTheClosedHierarchy)
{
    auto a = analyzeMain("x: A;\ny: B;\nc: C;\nz: P := c;\nx f;\nx g;\ny f;\nz h;\nreturn 0;", "",
                         "@[protocol] class P { msg h [int32]; }\n"
                         "class A { msg f [int32] { return 1; } msg g [int32] { return 2; } }\n"
                         "class B inherits A { msg f [int32] { return 3; } }\n"
                         "class C implements P { msg h [int32] { return 4; } }");
    EXPECT_EQ(a->diagnostics.errorCount(), 0u) << a->text();
    EXPECT_EQ(kindOfSend(*a, "f", "A"), bob::sema::SendKind::vtableCall);
    EXPECT_EQ(kindOfSend(*a, "g", "A"), bob::sema::SendKind::directMethodCall);
    EXPECT_EQ(kindOfSend(*a, "f", "B"), bob::sema::SendKind::directMethodCall);
    EXPECT_EQ(kindOfSend(*a, "h", "P"), bob::sema::SendKind::dynamicSend);
}

TEST(Semantic, ProtocolConformanceIsChecked)
{
    auto a = analyzeMain("return 0;", "",
                         "@[protocol] class P { msg h [int32]; msg k [int32] { return 1; } }\n"
                         "class Missing implements P { }\n"
                         "class Wrong implements P { msg h [int64] { return 1; } }\n"
                         "@[protocol] class WithField { public { x: int32; } }");
    EXPECT_TRUE(a->hasCode("sema.missingRequiredMessage")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.overrideMismatch")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.protocolField")) << a->text();
}

TEST(Semantic, OverridesMustKeepTheSignature)
{
    auto a = analyzeMain("return 0;", "", "class A { msg f: x:int32 [int32] { return x; } }\nclass B inherits A { msg f: x:int64 [int32] { return 1; } }");
    EXPECT_TRUE(a->hasCode("sema.overrideMismatch")) << a->text();
}

TEST(Semantic, FieldsArePrivateToTheClass)
{
    auto a = analyzeMain("c: C;\nv := c.secret;\nw := c secret;\nu := c open;\nc open: 3;\nreturn 0;", "",
                         "class C { private { secret: int32; } public { open: int32; } }");
    EXPECT_TRUE(a->hasCode("sema.privateField")) << a->text();
    EXPECT_EQ(a->diagnostics.errorCount(), 2u) << a->text();
}

TEST(Semantic, ReferencesAreNeverNilUnlessOptional)
{
    auto a = analyzeMain("x: C;\ny: C := nil;\nz: C? := nil;\nz f;\nreturn 0;", "", "class C { msg f { } }\n@[protocol] class Q { }\nclass D { private { q: Q; } }");
    EXPECT_TRUE(a->hasCode("sema.uninitializedReference")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.typeMismatch")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.optionalReceiver")) << a->text();
}

TEST(Semantic, ClassRulesMisc)
{
    auto a = analyzeMain("p: P;\nreturn 0;", "",
                         "@[protocol] class P { }\nclass C inherits P { }\nrecord R { c: P; }");
    EXPECT_TRUE(a->hasCode("sema.newProtocol")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.invalidBase")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.referenceInRecord")) << a->text();
}

TEST(Semantic, MiscTypeRules)
{
    auto a = analyzeMain("u: uint8 := 3;\nn := u negated;\nr := 2.5 % 1.0;\nk := 1 to: 3;\nt := p;\nreturn 0;");
    EXPECT_TRUE(a->hasCode("sema.unsignedNegate")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.intervalOutsideFor")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.cannotInfer")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.unsupported")) << a->text();
}

TEST(Semantic, EnumsAreDistinctAndCasesExhaustive)
{
    auto a = analyzeMain("c := Color red;\nn: int32 := c;\nd := Color purple;\ncase c of { Color red: { } }\nreturn 0;", "",
                         "enum Color { red; green; }\nenum Bad : uint8 { a := 255; b; }\nenum Twice { x := 1; y := 1; }");
    EXPECT_TRUE(a->hasCode("sema.typeMismatch")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.unknownMember")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.caseNotExhaustive")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.literalOutOfRange")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.duplicateEnumValue")) << a->text();
    EXPECT_NE(a->text().find("missing: green"), std::string::npos) << a->text();
}

TEST(Semantic, SubrangesCheckConstantsAndDistinctTypesDontMix)
{
    auto a = analyzeMain("p: Percent := 150;\nq: Percent := 50;\nwide := q + 1;\nphys := PhysAddr fromValue: 1u64;\nv: VirtAddr := phys;\nreturn 0;", "",
                         "type Percent := 0 to: 100;\ntype PhysAddr := uint64;\ntype VirtAddr := uint64;");
    EXPECT_TRUE(a->hasCode("sema.literalOutOfRange")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.typeMismatch")) << a->text();
    EXPECT_EQ(localNamed(*a, "wide")->type->name, "uint8");
    EXPECT_EQ(localNamed(*a, "q")->type->name, "Percent");
}

TEST(Semantic, ContainerElementTypesAreInferred)
{
    auto a = analyzeMain("a := [1, 2];\nb: array;\nb add: \"x\";\nm := [7, 1.5];\nd: dictionary;\nd at: \"k\" put: 2.5;\n"
                         "unused: array;\nreturn 0;");
    EXPECT_EQ(bob::sema::typeName(localNamed(*a, "a")->type), "array of: int32");
    EXPECT_EQ(bob::sema::typeName(localNamed(*a, "b")->type), "array of: string");
    EXPECT_EQ(bob::sema::typeName(localNamed(*a, "m")->type), "array of: real64");
    EXPECT_EQ(bob::sema::typeName(localNamed(*a, "d")->type), "dictionary of: real64");
    EXPECT_TRUE(a->hasCode("sema.cannotInferElement")) << a->text();
}

TEST(Semantic, StringsHaveViewsNotLengths)
{
    auto a = analyzeMain("s := \"hi\";\nn := s count;\nb := s bytes count;\nk := s + 1;\nfor x in s { }\nreturn 0;");
    EXPECT_TRUE(a->hasCode("sema.unknownMessage")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.typeMismatch")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.notIterable")) << a->text();
    EXPECT_EQ(localNamed(*a, "b")->type->name, "int64");
}

TEST(Semantic, MixedElementsNeedACommonType)
{
    auto a = analyzeMain("m := [1, \"a\", 2];\nok := [1, 2.5];\nsq: Square;\nci: Circle;\nshapes := [sq, ci];\nreturn 0;", "",
                         "@[protocol] class Shape { msg area [int32]; }\n"
                         "class Square implements Shape { msg area [int32] { return 4; } }\n"
                         "class Circle implements Shape { msg area [int32] { return 3; } }");
    EXPECT_NE(a->text().find("these elements have no common type ('int32', 'string')"), std::string::npos) << a->text();
    EXPECT_TRUE(a->hasCode("sema.mixedElementTypes")) << a->text();
    EXPECT_EQ(a->diagnostics.errorCount(), 1u) << a->text();
    EXPECT_EQ(bob::sema::typeName(localNamed(*a, "ok")->type), "array of: real64");
    EXPECT_EQ(bob::sema::typeName(localNamed(*a, "shapes")->type), "array of: Shape"); // a shared protocol
}

TEST(Semantic, MutatingSendsNeedAVariable)
{
    auto a = analyzeMain("[1, 2] add: 3;\nreturn 0;", "@[static] msg make [array of: int32] { return [1]; }");
    EXPECT_TRUE(a->hasCode("sema.notAssignable")) << a->text();
}

TEST(Semantic, ObjectsAreBuiltFromPublicFields)
{
    auto a = analyzeMain("a := Account owner: alice;\n"
                         "x := Account new;\n"
                         "y := Account balance: 1 owner: alice;\n"
                         "z := Account secret: 3;\n"
                         "h := Holder;\n"
                         "missing: Holder;\n"
                         "k := Holder shape: s;\n"
                         "c := a copy;\n"
                         "a init;\n"
                         "return 0;",
                         "",
                         "@[protocol] class Shape { msg area [int32]; }\n"
                         "class Square implements Shape { msg area [int32] { return 1; } }\n"
                         "class Person { }\n"
                         "class Account { public { owner: Person; balance: int64; } private { secret: int32; } msg init { } }\n"
                         "class Holder { public { shape: Shape; } }\n"
                         "class Bad { msg init: x:int32 { } }");
    EXPECT_TRUE(a->hasCode("sema.undeclared")) << a->text();   // alice, s
    EXPECT_TRUE(a->hasCode("sema.unknownMessage")) << a->text(); // new, secret:, init
    EXPECT_TRUE(a->hasCode("sema.constructorOrder")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.uninitializedReference")) << a->text(); // missing: Holder
    EXPECT_TRUE(a->hasCode("sema.invalidInit")) << a->text();
    std::string text = a->text();
    EXPECT_NE(text.find("there's no 'new'"), std::string::npos) << text;
    EXPECT_NE(text.find("Holder shape: …"), std::string::npos) << text;
    const bob::sema::LocalInfo* copy = localNamed(*a, "c");
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->type->name, "Account");
}

TEST(Semantic, RequiredFieldsMustBePassed)
{
    auto a = analyzeMain("sq: Square;\nok := Holder shape: sq;\nbad := Holder count: 1;\nreturn 0;", "",
                         "@[protocol] class Shape { msg area [int32]; }\n"
                         "class Square implements Shape { msg area [int32] { return 1; } }\n"
                         "class Holder { public { shape: Shape; count: int32; } }\n"
                         "class Hidden { private { shape: Shape; } }");
    EXPECT_TRUE(a->hasCode("sema.missingConstructorArgument")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.uninitializedReference")) << a->text(); // Hidden's private required field
    EXPECT_EQ(a->diagnostics.errorCount(), 2u) << a->text();
}

namespace {

bool onStack(const Analyzed& analyzed, const std::string& local)
{
    for (const auto& [id, object] : analyzed.model.stackObjects) {
        if (object.local->name == local) {
            return true;
        }
    }
    return false;
}

const char* escapeClasses =
    "@[protocol] class Sink { msg take: n:Node; }\n"
    "class Node { public { value: int32; next: Node?; } msg value2 [int32] { return value * 2; } }\n"
    "class Keeper implements Sink { private { kept: Node?; } msg take: n:Node { kept := n; } }\n"
    "class Looker implements Sink { msg take: n:Node { n value; } }\n"
    "class Registry { private { all: array of: Node; } msg add: n:Node { all add: n; } }\n"
    "class SelfKeeper { private { me: SelfKeeper?; } msg init { me := self; } }\n";

}

TEST(Semantic, ObjectsThatDontEscapeGoOnTheStack)
{
    auto a = analyzeMain("local: Node;\nlocal value: 3;\nn := local value2;\n"
                         "copied: Node;\nalias := copied;\nalias value;\n"
                         "kept := Node value: 1;\nholder: Node;\nholder next: kept;\n"
                         "stored: Node;\nregistry: Registry;\nregistry add: stored;\n"
                         "back: Node;\n"
                         "viaProtocol: Node;\nsink: Keeper;\ns: Sink := sink;\ns take: viaProtocol;\n"
                         "selfish: SelfKeeper;\n"
                         "outer: Node?;\nfor i in 1 to: 3 {\n  inner: Node;\n  outer := inner;\n}\n"
                         "loopLocal := 0;\nfor j in 1 to: 3 {\n  tmp := Node value: j;\n  loopLocal := loopLocal + tmp value;\n}\n"
                         "return self give: back;",
                         "@[static] msg give: n:Node [int32] { return n value; }", escapeClasses);
    EXPECT_EQ(a->diagnostics.errorCount(), 0u) << a->text();
    EXPECT_TRUE(onStack(*a, "local"));
    EXPECT_TRUE(onStack(*a, "copied"));    // its alias doesn't escape either
    EXPECT_FALSE(onStack(*a, "kept"));     // stored into holder's field
    EXPECT_TRUE(onStack(*a, "holder"));
    EXPECT_FALSE(onStack(*a, "stored"));   // Registry add: keeps it in a container
    EXPECT_TRUE(onStack(*a, "back"));      // `give:` only reads it
    EXPECT_FALSE(onStack(*a, "viaProtocol")); // some implementation of `take:` keeps it
    EXPECT_FALSE(onStack(*a, "selfish"));  // its init keeps self
    EXPECT_FALSE(onStack(*a, "inner"));    // assigned to a variable that outlives the loop
    EXPECT_TRUE(onStack(*a, "tmp"));
}

TEST(Semantic, StackPlacementCanBeRequiredOrRefused)
{
    auto a = analyzeMain("@[stack] ok: Node;\n@[stack] bad: Node;\nholder: Node;\nholder next: bad;\n"
                         "@[heap] big: Node;\n@[stack] n := 5;\n@[stack] @[heap] both: Node;\nreturn 0;",
                         "", escapeClasses);
    EXPECT_TRUE(a->hasCode("sema.invalidAttribute")) << a->text();
    std::string text = a->text();
    EXPECT_NE(text.find("applies to a declaration that creates an object"), std::string::npos) << text;
    EXPECT_NE(text.find("either @[stack] or @[heap]"), std::string::npos) << text;

    auto b = analyzeMain("@[stack] ok: Node;\n@[stack] bad: Node;\nholder: Node;\nholder next: bad;\n@[heap] big: Node;\nreturn 0;", "",
                         escapeClasses);
    EXPECT_TRUE(b->hasCode("sema.stackEscape")) << b->text();
    EXPECT_NE(b->text().find("'bad' is @[stack], but the object escapes: stored into a field"), std::string::npos) << b->text();
    EXPECT_TRUE(onStack(*b, "ok"));
    EXPECT_FALSE(onStack(*b, "big"));
}

TEST(Semantic, LocalsHoldingOnlyStackObjectsAreKnown)
{
    auto a = analyzeMain("pt := Node value: 1;\nq := pt;\nmaybe: Node? := nil;\nmaybe := q;\n"
                         "maybe ifNotNil: { [m] m value; };\nheld: Node;\nother: Node? := held;\nholder: Node;\nholder next: held;\n"
                         "mixed := Node value: 2;\nmixed := self make;\nreturn 0;",
                         "@[static] msg make [Node] { n := Node value: 3; return n; }", escapeClasses);
    EXPECT_EQ(a->diagnostics.errorCount(), 0u) << a->text();
    auto stackOnly = [&](const std::string& name) {
        const bob::sema::LocalInfo* local = localNamed(*a, name);
        return local != nullptr && a->model.stackOnlyLocals.count(local) != 0;
    };
    EXPECT_TRUE(stackOnly("pt"));
    EXPECT_TRUE(stackOnly("q"));
    EXPECT_TRUE(stackOnly("maybe"));
    EXPECT_TRUE(stackOnly("m"));
    EXPECT_FALSE(stackOnly("held"));  // escapes into holder's field, so it's on the heap
    EXPECT_FALSE(stackOnly("other"));
    EXPECT_FALSE(stackOnly("mixed")); // also holds a message result
    EXPECT_FALSE(stackOnly("n"));     // returned
}

TEST(Semantic, CompileTimeCodeRules)
{
    auto a = analyzeMain("x := self table;\ncompiler note: \"hi\";\nLIMIT := 3;\nn := 5;\nk := @[compiletime] (n + 1);\nreturn 0;",
                         "@[static] @[compiletime] msg table [int32] { return 1; }",
                         "@[constant] LIMIT := 10;\n@[constant] OBJECT := Box value: 1;\nclass Box { public { value: int32; } }\n"
                         "@[constant] typed: uint8 := 300;\n@[constant] untyped: int32;\nGLOBAL := 1;");
    EXPECT_TRUE(a->hasCode("sema.compileTimeOnly")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.compilerOutsideCompileTime")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.assignToConstant")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.undeclared")) << a->text(); // n isn't visible in compile-time code
    EXPECT_TRUE(a->hasCode("sema.notEmbeddable")) << a->text();
    EXPECT_TRUE(a->hasCode("sema.literalOutOfRange")) << a->text(); // 300 isn't a uint8
    EXPECT_TRUE(a->hasCode("sema.invalidConstant")) << a->text();   // a constant needs a value
    EXPECT_TRUE(a->hasCode("sema.unitLevelVariable")) << a->text(); // no @[constant]
}

TEST(Semantic, MetaIsCompileTimeInformation)
{
    auto a = analyzeMain("pt: Point;\ns := pt meta size;\nt := pt meta typeName;\nn := pt meta name;\nc := Color meta count;\nm := Color meta members;\n"
                         "e := [1, 2] meta elementType typeName;\nreturn 0;",
                         "", "record Point { x: int32; y: int8; }\nenum Color { red; green; }");
    EXPECT_EQ(a->diagnostics.errorCount(), 0u) << a->text();
    auto constantOf = [&](const std::string& name) -> const bob::sema::ConstValue* {
        const bob::sema::LocalInfo* local = localNamed(*a, name);
        auto assignment = local != nullptr ? dynamic_cast<const bob::ast::Assignment*>(local->decl) : nullptr;
        const bob::sema::SendInfo* send = assignment != nullptr ? a->model.sendOf(assignment->value) : nullptr;
        return send != nullptr ? send->value : nullptr;
    };
    ASSERT_NE(constantOf("s"), nullptr);
    EXPECT_EQ(constantOf("s")->bits, 8u);
    EXPECT_EQ(constantOf("t")->text, "Point");
    EXPECT_EQ(constantOf("n")->text, "pt");
    EXPECT_EQ(constantOf("c")->bits, 2u);
    EXPECT_EQ(constantOf("m")->elements.size(), 2u);
    EXPECT_EQ(constantOf("e")->text, "int32");
}

TEST(Semantic, ReflectionThroughTheClassObject)
{
    auto a = analyzeMain("b: Box;\nc := b class;\nn := c name;\nk := Box class isKindOf: c;\nr := c respondsTo: \"size\";\ns := c superclass;\n"
                         "stack := b isOnStack;\nd: Class;\nbad := Shape class;\nreturn 0;",
                         "",
                         "@[protocol] class Shape { msg area [int32]; }\n"
                         "@[reflect: invoke] class Box { public { size: int32; } msg class [int32] { return 1; } }\n"
                         "@[reflect: everything] class Odd { }\n"
                         "class Kept { public { kind: Class; } }\n"
                         "record R { kind: Class; }");
    EXPECT_EQ(localNamed(*a, "c")->type->name, "Class");
    EXPECT_EQ(localNamed(*a, "n")->type->name, "string");
    EXPECT_EQ(localNamed(*a, "k")->type->name, "bool");
    EXPECT_EQ(localNamed(*a, "r")->type->name, "bool");
    EXPECT_EQ(localNamed(*a, "s")->type->name, "Class?");
    EXPECT_EQ(localNamed(*a, "stack")->type->name, "bool");
    EXPECT_TRUE(a->hasCode("sema.reservedSelector")) << a->text();
    EXPECT_NE(a->text().find("takes a level: none, identity, introspect or invoke"), std::string::npos) << a->text();
    EXPECT_NE(a->text().find("'d' is a Class and has no default"), std::string::npos) << a->text();
    EXPECT_NE(a->text().find("'Shape' is a protocol and has no class object"), std::string::npos) << a->text();
    EXPECT_NE(a->text().find("a Class has no default, so it can't be a record field"), std::string::npos) << a->text();
    EXPECT_EQ(a->diagnostics.errorCount(), 5u) << a->text();
    EXPECT_EQ(a->model.classNamed("Box")->reflectLevel, bob::sema::ReflectLevel::invoke);
    EXPECT_EQ(a->model.classNamed("Kept")->reflectLevel, bob::sema::ReflectLevel::introspect); // the default
    EXPECT_TRUE(a->model.classNamed("Kept")->field("kind")->required); // a Class field must be passed
}

TEST(Semantic, IntrospectionThroughTheClassObject)
{
    auto a = analyzeMain("b: Box;\nc := b class;\nfs := c fields;\nms := c messages;\nps := c protocols;\nct := Box meta fields;\n"
                         "w: int32 := c get: \"size\" of: b;\nok: int32 := Box class get: \"size\" of: b;\n"
                         "wrong: int64 := Box class get: \"size\" of: b;\nmissing: int32 := Box class get: \"nope\" of: b;\n"
                         "hidden: int32 := Box class get: \"secret\" of: b;\nq: Quiet;\nnone: int32 := Quiet class get: \"n\" of: q;\n"
                         "notObject: int32 := c get: \"size\" of: 3;\nx := 1 + (c get: \"size\" of: b);\n(c get: \"size\" of: b);\nreturn 0;",
                         "",
                         "class Box { public { size: int32; } private { secret: int32; } }\n"
                         "@[reflect: identity] class Quiet { public { n: int32; } }");
    EXPECT_EQ(localNamed(*a, "fs")->type->name, "array of: FieldMeta");
    EXPECT_EQ(localNamed(*a, "ms")->type->name, "array of: MessageMeta");
    EXPECT_EQ(localNamed(*a, "ps")->type->name, "array of: string");
    EXPECT_EQ(localNamed(*a, "ct")->type, localNamed(*a, "fs")->type); // one FieldMeta for compile time and runtime
    EXPECT_NE(a->text().find("'Box size' is a 'int32', not a 'int64'"), std::string::npos) << a->text();
    EXPECT_NE(a->text().find("'Box' has no public field 'nope'"), std::string::npos) << a->text();
    EXPECT_NE(a->text().find("'Box' has no public field 'secret'"), std::string::npos) << a->text();
    EXPECT_NE(a->text().find("'Quiet' is emitted at @[reflect: identity]"), std::string::npos) << a->text();
    EXPECT_NE(a->text().find("reads a field of an object, not a"), std::string::npos) << a->text();
    EXPECT_TRUE(a->hasCode("sema.reflectNeedsType")) << a->text();
}

TEST(Semantic, InvokeLevelReflectionNeedsAReflectorOnMain)
{
    // Without a Reflector on main, the first invoke-level send is reported.
    auto a = analyzeMain("b: Box;\nc := b class;\nn: int32 := c perform: \"size\" on: b;\nreturn 0;", "",
                         "@[reflect: invoke] class Box { public { size: int32; } }");
    EXPECT_TRUE(a->hasCode("sema.reflectorRequired")) << a->text();
    EXPECT_EQ(a->diagnostics.errorCount(), 1u) << a->text();
    EXPECT_TRUE(a->model.needsReflector);

    // Reading needs nothing.
    auto reading = analyzeMain("b: Box;\nn: int32 := b class get: \"size\" of: b;\nreturn 0;", "", "class Box { public { size: int32; } }");
    EXPECT_EQ(reading->diagnostics.errorCount(), 0u) << reading->text();
    EXPECT_FALSE(reading->model.needsReflector);

    // Reflector is only main's parameter type; compile-time code can't act through reflection.
    auto misplaced = analyzeMain("return 0;", "@[static] msg helper: r:Reflector [int32] { return 0; }\n"
                                              "@[static] @[compiletime] msg early [int32] { c := Box class; b: Box := c new; return 0; }",
                                 "@[reflect: invoke] class Box { public { size: int32 := 1; } }");
    EXPECT_TRUE(misplaced->hasCode("sema.invalidReflector")) << misplaced->text();
    EXPECT_NE(misplaced->text().find("'new' acts through reflection, which compile-time code can't do"), std::string::npos) << misplaced->text();
}

TEST(Semantic, InvokeLevelReflectionIsCheckedOnANamedClass)
{
    std::string source = "program p, 1.0.0\n{\n"
                         "@[reflect: invoke] class Box { public { size: int32 := 1; } msg grow: n:int64 [int64] { return n; } }\n"
                         "class Plain { public { n: int32 := 1; } msg get [int32] { return n; } }\n"
                         "@[reflect: invoke] class Tagged { public { kind: Class; } }\n"
                         "extends p {\n@[static] msg main: args:array reflector: r:Reflector [int32] {\n"
                         "b: Box;\npl: Plain;\n"
                         "g := Box class perform: \"grow:\" on: b with: 2;\n"
                         "Box class perform: \"shrink\" on: b;\n"
                         "Box class perform: \"grow:\" on: b;\n"
                         "Plain class perform: \"get\" on: pl;\n"
                         "Box class set: \"size\" of: b to: 3;\n"
                         "Plain class set: \"n\" of: pl to: 3;\n"
                         "made: Box := Box class new;\n"
                         "notBox: Box := Plain class new;\n"
                         "t: Tagged := Tagged class new;\n"
                         "untyped := Box class new;\n"
                         "found := Class named: \"p.Box\";\n"
                         "c := b class;\nc perform: \"grow:\" on: b with: 2i64 with: 3;\n"
                         "return 0;\n}\n}\n}\n";
    bob::DiagnosticEngine diagnostics;
    bob::parser::ParsedUnit parsed = bob::parser::parseSource(source, diagnostics);
    bob::sema::SemanticModel model;
    bob::sema::analyze(parsed.unit, model, diagnostics);
    std::string text;
    for (const auto& diagnostic : diagnostics.diagnostics()) {
        text += diagnostic.code + ": " + diagnostic.message + "\n";
    }
    auto has = [&](const std::string& part) { return text.find(part) != std::string::npos; };
    EXPECT_TRUE(has("'Box' doesn't understand 'shrink'")) << text;
    EXPECT_TRUE(has("'grow:' takes 1 argument(s), but 0 are passed")) << text;
    EXPECT_TRUE(has("'Plain' is emitted at @[reflect: introspect]; 'perform:' needs 'invoke'")) << text;
    EXPECT_TRUE(has("writing a field by reflection needs 'invoke'")) << text;
    EXPECT_TRUE(has("a new 'Plain' isn't a 'Box'")) << text;
    EXPECT_TRUE(has("'Tagged' can't be created by reflection")) << text;
    EXPECT_TRUE(has("'new' takes its type from context")) << text;
    EXPECT_TRUE(has("'grow:' takes 1 argument(s), but 2 are passed")) << text;
    EXPECT_FALSE(has("reflectorRequired")) << text;
    EXPECT_EQ(diagnostics.errorCount(), 8u) << text;
    for (const auto& local : model.locals) {
        if (local->name == "g") {
            EXPECT_EQ(local->type->name, "int64"); // the message's own result type
        }
        if (local->name == "found") {
            EXPECT_EQ(local->type->name, "Class?");
        }
    }
}

TEST(Semantic, ContainersOfBlocks)
{
    auto a = analyzeMain("f := { [x: int32] x };\nops: array of: { int32 [int32] };\nops add: { [x] x + 1 };\nops add: f;\n"
                         "same: array of: { int32 [int32] } := ops;\n"
                         "table: dictionary of: { int32 [int32] };\nhit := table at: \"a\";\n"
                         "bad := ops includes: f;\n"
                         "later: array;\nlater add: { [x] x };\n"
                         "fixed: array of: { int32 [int32] } by: Slot;\n"
                         "keyed: hashtable keys: { int32 [int32] } values: int32;\n"
                         "loose: array of: block;\nreturn 0;",
                         "", "enum Slot { a; b; }");
    EXPECT_EQ(localNamed(*a, "same")->type, localNamed(*a, "ops")->type); // one array type per block shape
    EXPECT_EQ(localNamed(*a, "hit")->type->name, "[{ int32 [int32] }]?");
    EXPECT_NE(a->text().find("'includes:' needs elements that '=' can compare"), std::string::npos) << a->text();
    EXPECT_NE(a->text().find("the type of block parameter 'x' can't be inferred here"), std::string::npos) << a->text();
    EXPECT_NE(a->text().find("a block has no default"), std::string::npos) << a->text();
    EXPECT_NE(a->text().find("can't be a key"), std::string::npos) << a->text();
    EXPECT_NE(a->text().find("'block' can't be an array element"), std::string::npos) << a->text();
}

TEST(Semantic, PointerRules)
{
    auto a = analyzeMain("x: int32 := 1;\nunguarded := x address;\nb: Box;\n"
                         "@[unsafe] {\n"
                         "  ok := x address;\n  t: Tight;\n  tb := t.b address;\n  s: string := \"a\";\n  sp := s address;\n"
                         "  needs: pointer to: int32;\n  nope := 5 asPointer;\n"
                         "  v: volatilePointer to: int32 := ok asPointer;\n  back: pointer to: int32 := v asPointer;\n  sl := v slice: 2;\n"
                         "  never := ok = nil;\n"
                         "}\nreturn 0;",
                         "@[static] @[unsafe] msg param: n:int32 [int32] { p := n address; return 0; }",
                         "@[packed] record Tight { a: uint8; b: uint32; }\nrecord Holds { p: pointer to: uint32; }\n"
                         "record Fine { p: [pointer to: uint32]?; }\nclass Box { public { n: int32 := 1; } }\n"
                         "class Strings { public { p: [pointer to: string]?; } }");
    auto has = [&](const char* text) { return a->text().find(text) != std::string::npos; };
    EXPECT_TRUE(has("'address' works with raw memory, so it needs an unsafe region")) << a->text();
    EXPECT_TRUE(has("a record's pointer field must be optional")) << a->text();
    EXPECT_TRUE(has("'n' is read-only, so it has no address")) << a->text();
    EXPECT_TRUE(has("may be unaligned, so it has no address")) << a->text();
    EXPECT_TRUE(has("a 'string' (reference counted or without a fixed layout) has no address")) << a->text();
    EXPECT_TRUE(has("'needs' is a pointer, which is never null")) << a->text();
    EXPECT_TRUE(has("'asPointer' takes its pointer type from context")) << a->text();
    EXPECT_TRUE(has("a volatilePointer can't become a plain pointer")) << a->text();
    EXPECT_TRUE(has("can't be made from a volatilePointer")) << a->text();
    EXPECT_TRUE(has("is never nil")) << a->text();
    EXPECT_TRUE(has("a pointer or slice points at plain data")) << a->text(); // pointer to: string
    EXPECT_TRUE(a->hasCode("sema.unsafeRequired")) << a->text();
}

TEST(Semantic, PointersToLocalsCantOutliveTheFrame)
{
    auto a = analyzeMain("return self fine - 4;",
                         "@[static] @[unsafe] msg leak [pointer to: int32] { x: int32 := 1; return x address; }\n"
                         "@[static] @[unsafe] msg stash: b:Box { x: int32 := 1; b keep: x address; }\n"
                         "@[static] @[unsafe] msg derived [pointer to: int32] { x: int32 := 1; px := x address; return px advancedBy: 0; }\n"
                         "@[static] @[unsafe] msg keeper: p:[pointer to: int32] { all: array of: [pointer to: int32]; all add: p; }\n"
                         "@[static] @[unsafe] msg passes { x: int32 := 1; self keeper: x address; }\n"
                         "@[static] @[unsafe] msg capture [{ [int32] }] { x: int32 := 1; px := x address; return { px contents }; }\n"
                         "@[static] @[unsafe] msg reader: p:[pointer to: int32] [int32] { return p contents; }\n"
                         "@[static] @[unsafe] msg fine [int32] { x: int32 := 4; return self reader: x address; }\n"
                         "@[static] @[unsafe] msg fromMemory: m:[pointer to: [pointer to: int32]] [pointer to: int32] { return m contents; }",
                         "class Box { public { keep: [pointer to: int32]?; } }");
    auto count = [&](const char* text) {
        size_t n = 0;
        for (size_t at = a->text().find(text); at != std::string::npos; at = a->text().find(text, at + 1)) {
            ++n;
        }
        return n;
    };
    EXPECT_EQ(count("[sema.pointerEscapes]"), 5u) << a->text(); // leak, stash, derived, passes, capture
    EXPECT_EQ(count("but it does: returned"), 2u) << a->text();
    EXPECT_EQ(count("but it does: stored into a field"), 1u) << a->text();
    EXPECT_EQ(count("but it does: passed to a message that keeps it"), 1u) << a->text();
    EXPECT_EQ(count("but it does: captured by a block that outlives its frame"), 1u) << a->text();
    EXPECT_EQ(a->diagnostics.errorCount(), 5u) << a->text(); // fine and fromMemory are accepted
}

TEST(Semantic, AlignmentRules)
{
    auto a = analyzeMain("lines: array of: Line;\ntable: dictionary of: Line;\nok: array of: Sixteen;\n@[align: 4096] page: array of: uint8 by: [0 to: 9];\n"
                         "@[align: 8] text: string := \"x\";\nreturn 0;",
                         "",
                         "@[align: 64] record Line { a: uint8; }\n@[align: 16] record Sixteen { a: uint8; }\n"
                         "@[align: 3] record Odd { a: uint8; }\n@[align: 8192] record Huge { a: uint8; }\n"
                         "@[packed] @[align: 8] record Both { a: uint8; }\n@[packed] record Inner { a: uint8; @[align: 4] b: uint32; }\n"
                         "class Holder { public { line: Line; } }\nclass Fine { public { s: Sixteen; } }");
    auto has = [&](const char* text) { return a->text().find(text) != std::string::npos; };
    EXPECT_TRUE(has("'Line' needs 64-byte alignment, but a container's buffer is only 16-byte aligned")) << a->text();
    EXPECT_TRUE(has("'Line' needs 64-byte alignment, but an object is only 16-byte aligned")) << a->text();
    EXPECT_TRUE(has("takes a power of two from 1 to 4096")) << a->text();
    EXPECT_TRUE(has("'@[align:]' together with '@[packed]' isn't supported yet")) << a->text();
    EXPECT_TRUE(has("'@[align:]' on a field of a @[packed] record isn't supported yet")) << a->text();
    EXPECT_TRUE(has("'@[align:]' places plain data")) << a->text();
    EXPECT_EQ(a->diagnostics.errorCount(), 8u) << a->text(); // two containers, the object, Odd, Huge, Both, Inner, the string
    EXPECT_EQ(localNamed(*a, "page")->align, 4096u);

    // A variable a heap closure keeps a copy of can't need more than 16 bytes.
    auto captured = analyzeMain("return 0;", "@[static] msg keep [{ [uint8] }] { line: Line; return { line.a }; }", "@[align: 64] record Line { a: uint8; }");
    EXPECT_TRUE(captured->hasCode("sema.alignTooLarge")) << captured->text();
}

TEST(Semantic, ExternCRules)
{
    auto a = analyzeMain("n := 3;\nt := self tick;\nf: Op := { [x] x + n };\ng: Op := { [x] x * 2 };\nr := g value: 1;\nmissing: Op;\n"
                         "@[unsafe] { ok := (self tick) asInt32 + (g value: 2); }\nreturn 0;",
                         "@[extern: \"C\", symbol: \"GetTickCount\"] msg tick [uint32];\n"
                         "@[extern: \"C\"] msg takesString: s:string;\n"
                         "@[extern: \"C\"] msg takesSlice: s:[slice of: uint8];\n"
                         "@[extern: \"C\"] msg exported: x:int32 [int32] { return x; }\n"
                         "@[extern: \"Pascal\"] msg wrong;\n"
                         "@[symbol: \"lonely\"] @[static] msg alone { }",
                         "type Op := cFunction { int32 [int32] };\nrecord Table { f: Op; }\nrecord Fine { f: Op?; }\n"
                         "type Bad := cFunction { string };");
    auto has = [&](const char* text) { return a->text().find(text) != std::string::npos; };
    EXPECT_TRUE(has("'GetTickCount' is a C function the compiler can't check")) << a->text();
    EXPECT_TRUE(has("can't use 'n' from the code around it")) << a->text();
    EXPECT_TRUE(has("a C function pointer may point anywhere, so calling it needs an unsafe region")) << a->text();
    EXPECT_TRUE(has("'missing' is a C function, which is never null")) << a->text();
    EXPECT_TRUE(has("a record's C function field must be optional")) << a->text();
    EXPECT_TRUE(has("'s' crosses to C, so it needs a C type, not 'string'; reference-counted values can't cross to C")) << a->text();
    EXPECT_TRUE(has("pass the slice's pointer and count separately")) << a->text();
    EXPECT_TRUE(has("a C function takes C types, not 'string'")) << a->text();
    EXPECT_TRUE(has("takes the calling convention \"C\"")) << a->text();
    EXPECT_TRUE(has("goes with '@[extern: \"C\"]'")) << a->text();
    EXPECT_EQ(a->diagnostics.errorCount(), 10u) << a->text();
    const bob::sema::MessageInfo* exported = a->model.program.bySelector.at("exported:");
    EXPECT_TRUE(exported->externC);
    EXPECT_FALSE(exported->externImport);
    EXPECT_EQ(exported->cSymbol, "exported");
    EXPECT_EQ(a->model.program.bySelector.at("tick")->cSymbol, "GetTickCount");
    EXPECT_EQ(localNamed(*a, "g")->type->name, "cFunction { int32 [int32] }");
}

TEST(Semantic, ReflectionLevelDefaultsAndTheBuildCap)
{
    bob::sema::AnalyzeOptions capped;
    capped.reflectMax = bob::sema::ReflectLevel::identity;
    auto a = analyzeMain("return 0;", "", "class Plain { }\n@[reflect: invoke] class Asks { }\n@[reflect: none] class Hidden { }", capped,
                         "@[reflect: none] ");
    EXPECT_EQ(a->diagnostics.errorCount(), 0u) << a->text();
    EXPECT_TRUE(a->hasCode("sema.reflectCapped")) << a->text();
    EXPECT_EQ(a->model.classNamed("Plain")->reflectLevel, bob::sema::ReflectLevel::none); // the unit's default
    EXPECT_EQ(a->model.classNamed("Asks")->reflectLevel, bob::sema::ReflectLevel::identity);
    EXPECT_EQ(a->model.classNamed("Hidden")->reflectLevel, bob::sema::ReflectLevel::none);
}
