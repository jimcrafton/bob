#include "syntaxTestSupport.h"

#include <gtest/gtest.h>

using bobTest::dumpFirstItem;
using bobTest::parse;

namespace {

std::string expectClean(const std::string& statements)
{
    std::string diagnostics;
    std::string dump = dumpFirstItem(statements, &diagnostics);
    EXPECT_EQ(diagnostics, "") << statements;
    return dump;
}

}

TEST(Parser, ArithmeticPrecedence)
{
    EXPECT_EQ(expectClean("t := 2 + 3 * 4;"),
              "Assign\n"
              "  target: Ident name=t\n"
              "  value: Binary selector=+\n"
              "    receiver: Int value=2\n"
              "    argument: Binary selector=*\n"
              "      receiver: Int value=3\n"
              "      argument: Int value=4\n");
}

TEST(Parser, BitwiseOperatorsAreWords)
{
    // `and` binds tighter than `xor` and `or`, as for bools.
    EXPECT_EQ(expectClean("x and mask xor 1;"),
              "ExprStmt\n"
              "  expression: Binary selector=xor\n"
              "    receiver: Binary selector=and\n"
              "      receiver: Ident name=x\n"
              "      argument: Ident name=mask\n"
              "    argument: Int value=1\n");
    for (const char* source : {"x & 1;", "x | 1;", "x ^ 1;"}) {
        std::string diagnostics;
        dumpFirstItem(source, &diagnostics);
        EXPECT_NE(diagnostics.find("bitwise operations are words"), std::string::npos) << diagnostics;
    }
}

TEST(Parser, BlockTypesAreWrittenInBraces)
{
    EXPECT_EQ(expectClean("f: { int32, string [bool] };"),
              "VarDecl name=f\n"
              "  type: BlockType\n"
              "    param: Type name=int32\n"
              "    param: Type name=string\n"
              "    result: Type name=bool\n");
    EXPECT_EQ(expectClean("g: { [array of: int32] }?;"),
              "VarDecl name=g\n"
              "  type: Optional\n"
              "    inner: BlockType\n"
              "      result: TypeMessage selector=of:\n"
              "        receiver: Type name=array\n"
              "        argument: Type name=int32\n");
    EXPECT_EQ(expectClean("h: { };"), "VarDecl name=h\n  type: BlockType\n");
}

TEST(Parser, ShiftsBindLikeMultiplication)
{
    EXPECT_EQ(expectClean("v := 1 + x shl 2;"),
              "Assign\n"
              "  target: Ident name=v\n"
              "  value: Binary selector=+\n"
              "    receiver: Int value=1\n"
              "    argument: Binary selector=shl\n"
              "      receiver: Ident name=x\n"
              "      argument: Int value=2\n");
}

TEST(Parser, NotEqualIsAComparison)
{
    EXPECT_EQ(expectClean("a + 1 <> b;"),
              "ExprStmt\n"
              "  expression: Binary selector=<>\n"
              "    receiver: Binary selector=+\n"
              "      receiver: Ident name=a\n"
              "      argument: Int value=1\n"
              "    argument: Ident name=b\n");
}

TEST(Parser, LogicalOperatorsAreLowest)
{
    EXPECT_EQ(expectClean("a < 1 or b and c;"),
              "ExprStmt\n"
              "  expression: Binary selector=or\n"
              "    receiver: Binary selector=<\n"
              "      receiver: Ident name=a\n"
              "      argument: Int value=1\n"
              "    argument: Binary selector=and\n"
              "      receiver: Ident name=b\n"
              "      argument: Ident name=c\n");
}

TEST(Parser, UnaryBindsTightest)
{
    EXPECT_EQ(expectClean("4 + 5 toString;"),
              "ExprStmt\n"
              "  expression: Binary selector=+\n"
              "    receiver: Int value=4\n"
              "    argument: Unary selector=toString\n"
              "      receiver: Int value=5\n");
}

TEST(Parser, KeywordSendJoinsSelector)
{
    EXPECT_EQ(expectClean("d at: i + 1 put: v name;"),
              "ExprStmt\n"
              "  expression: Keyword selector=at:put:\n"
              "    receiver: Ident name=d\n"
              "    argument: Binary selector=+\n"
              "      receiver: Ident name=i\n"
              "      argument: Int value=1\n"
              "    argument: Unary selector=name\n"
              "      receiver: Ident name=v\n");
}

TEST(Parser, ContextualWordsAreOrdinaryMessages)
{
    EXPECT_EQ(expectClean("k := obj class;"),
              "Assign\n"
              "  target: Ident name=k\n"
              "  value: Unary selector=class\n"
              "    receiver: Ident name=obj\n");
}

TEST(Parser, NegativeLiteralAndSubtraction)
{
    EXPECT_EQ(expectClean("x := a -1 max: -2;"),
              "Assign\n"
              "  target: Ident name=x\n"
              "  value: Keyword selector=max:\n"
              "    receiver: Binary selector=-\n"
              "      receiver: Ident name=a\n"
              "      argument: Int value=1\n"
              "    argument: Int value=-2\n");
}

TEST(Parser, Declarations)
{
    EXPECT_EQ(expectClean("x: int32;"), "VarDecl name=x\n  type: Type name=int32\n");
    EXPECT_EQ(expectClean("y : int32 := 4;"), "VarDecl name=y\n  type: Type name=int32\n  init: Int value=4\n");
    EXPECT_EQ(expectClean("p: Foo? := nil;"), "VarDecl name=p\n  type: Optional\n    inner: Type name=Foo\n  init: Nil\n");
}

TEST(Parser, FieldAccessAssignment)
{
    EXPECT_EQ(expectClean("p.hdr.kind := 6;"),
              "Assign\n"
              "  target: FieldAccess field=kind\n"
              "    receiver: FieldAccess field=hdr\n"
              "      receiver: Ident name=p\n"
              "  value: Int value=6\n");
}

TEST(Parser, ReturnWithSeveralValues)
{
    EXPECT_EQ(expectClean("return 100, \"hello\";"), "Return\n  value: Int value=100\n  value: String value=\"hello\"\n");
}

TEST(Parser, BlockWithTypedParameter)
{
    EXPECT_EQ(expectClean("sq := { :i: int32 :j | i * j };"),
              "Assign\n"
              "  target: Ident name=sq\n"
              "  value: Block\n"
              "    param: BlockParam name=i\n"
              "      type: Type name=int32\n"
              "    param: BlockParam name=j\n"
              "    statement: ExprStmt\n"
              "      expression: Binary selector=*\n"
              "        receiver: Ident name=i\n"
              "        argument: Ident name=j\n");
}

TEST(Parser, BlockParametersWithoutTypes)
{
    EXPECT_EQ(expectClean("f := { :a :b | a };"),
              "Assign\n"
              "  target: Ident name=f\n"
              "  value: Block\n"
              "    param: BlockParam name=a\n"
              "    param: BlockParam name=b\n"
              "    statement: ExprStmt\n"
              "      expression: Ident name=a\n");
}

TEST(Parser, BlockReceiverOnSameLine)
{
    EXPECT_EQ(expectClean("{ i < 10 } whileTrue: { i := i + 1; };"),
              "ExprStmt\n"
              "  expression: Keyword selector=whileTrue:\n"
              "    receiver: Block\n"
              "      statement: ExprStmt\n"
              "        expression: Binary selector=<\n"
              "          receiver: Ident name=i\n"
              "          argument: Int value=10\n"
              "    argument: Block\n"
              "      statement: Assign\n"
              "        target: Ident name=i\n"
              "        value: Binary selector=+\n"
              "          receiver: Ident name=i\n"
              "          argument: Int value=1\n");
}

TEST(Parser, StatementGroupEndsAtNewline)
{
    auto parsed = parse("{\n@[overflow: wrap] {\n a := a + b;\n}\nx: int32;\nn := 1;\n}");
    EXPECT_EQ(parsed->diagnosticsText(), "");
    ASSERT_EQ(parsed->unit.unit->items.size(), 3u);
    EXPECT_EQ(bob::ast::dumpAstText(parsed->unit.unit->items[0]),
              "Group\n"
              "  attribute: Attribute name=overflow\n"
              "    value: Ident name=wrap\n"
              "  statement: Assign\n"
              "    target: Ident name=a\n"
              "    value: Binary selector=+\n"
              "      receiver: Ident name=a\n"
              "      argument: Ident name=b\n");
    EXPECT_EQ(parsed->unit.unit->items[1]->kind, bob::ast::NodeKind::variableDecl);
}

TEST(Parser, AttributedExpression)
{
    EXPECT_EQ(expectClean("h := @[overflow: wrap] (h * 31);"),
              "Assign\n"
              "  target: Ident name=h\n"
              "  value: AttributedExpr\n"
              "    attribute: Attribute name=overflow\n"
              "      value: Ident name=wrap\n"
              "    expression: Binary selector=*\n"
              "      receiver: Ident name=h\n"
              "      argument: Int value=31\n");
}

TEST(Parser, CaseStatementWithPascalLabels)
{
    EXPECT_EQ(expectClean("case c of {\n Color red, Color green: { self go; }\n (1 to: 9): { }\n int32: { :n | n }\n} else {\n x := 1;\n}"),
              "Case\n"
              "  subject: Ident name=c\n"
              "  entry: CaseEntry\n"
              "    label: Unary selector=red\n"
              "      receiver: Ident name=Color\n"
              "    label: Unary selector=green\n"
              "      receiver: Ident name=Color\n"
              "    body: Block\n"
              "      statement: ExprStmt\n"
              "        expression: Unary selector=go\n"
              "          receiver: Self\n"
              "  entry: CaseEntry\n"
              "    label: Keyword selector=to:\n"
              "      receiver: Int value=1\n"
              "      argument: Int value=9\n"
              "    body: Block\n"
              "  entry: CaseEntry\n"
              "    label: Ident name=int32\n"
              "    body: Block\n"
              "      param: BlockParam name=n\n"
              "      statement: ExprStmt\n"
              "        expression: Ident name=n\n"
              "  else: Group\n"
              "    statement: Assign\n"
              "      target: Ident name=x\n"
              "      value: Int value=1\n");
}

TEST(Parser, CaseSubjectCanBeAnExpression)
{
    std::string dump = expectClean("case self state next of { Color blue : { } 0: { } }");
    EXPECT_NE(dump.find("subject: Unary selector=next\n    receiver: Unary selector=state"), std::string::npos) << dump;
    EXPECT_NE(dump.find("label: Unary selector=blue"), std::string::npos) << dump;
    EXPECT_NE(dump.find("label: Int value=0"), std::string::npos) << dump;
}

TEST(Parser, CaseNeedsOf)
{
    std::string diagnostics;
    dumpFirstItem("case c { }", &diagnostics);
    EXPECT_NE(diagnostics.find("'of' after the case subject"), std::string::npos) << diagnostics;
}

TEST(Parser, IfElseIfElse)
{
    EXPECT_EQ(expectClean("if a > b {\n m := a;\n} else if a = b {\n} else {\n m := b;\n}"),
              "If\n"
              "  condition: Binary selector=>\n"
              "    receiver: Ident name=a\n"
              "    argument: Ident name=b\n"
              "  then: Group\n"
              "    statement: Assign\n"
              "      target: Ident name=m\n"
              "      value: Ident name=a\n"
              "  else: If\n"
              "    condition: Binary selector==\n"
              "      receiver: Ident name=a\n"
              "      argument: Ident name=b\n"
              "    then: Group\n"
              "    else: Group\n"
              "      statement: Assign\n"
              "        target: Ident name=m\n"
              "        value: Ident name=b\n");
}

TEST(Parser, IfConditionWithKeywordSend)
{
    EXPECT_EQ(expectClean("if items includes: x {\n}"),
              "If\n"
              "  condition: Keyword selector=includes:\n"
              "    receiver: Ident name=items\n"
              "    argument: Ident name=x\n"
              "  then: Group\n");
}

TEST(Parser, WhileLoopBreakContinue)
{
    EXPECT_EQ(expectClean("while i < 10 {\n continue;\n}\nloop {\n break;\n}"),
              "While\n"
              "  condition: Binary selector=<\n"
              "    receiver: Ident name=i\n"
              "    argument: Int value=10\n"
              "  body: Group\n"
              "    statement: Continue\n");
    auto parsed = parse("{ loop { break; } }");
    EXPECT_EQ(bob::ast::dumpAstText(parsed->unit.unit->items.front()), "Loop\n  body: Group\n    statement: Break\n");
}

TEST(Parser, ForInWithOptionalType)
{
    EXPECT_EQ(expectClean("for i in 1 to: 10 {\n}"),
              "For variable=i\n"
              "  iterable: Keyword selector=to:\n"
              "    receiver: Int value=1\n"
              "    argument: Int value=10\n"
              "  body: Group\n");
    EXPECT_EQ(expectClean("for c: Color in Color {\n}"),
              "For variable=c\n"
              "  type: Type name=Color\n"
              "  iterable: Ident name=Color\n"
              "  body: Group\n");
    EXPECT_EQ(expectClean("for b : uint8 in bytes {\n}"),
              "For variable=b\n"
              "  type: Type name=uint8\n"
              "  iterable: Ident name=bytes\n"
              "  body: Group\n");
}

TEST(Parser, ControlBodiesNeedBraces)
{
    const char* cases[] = {"if a b := 1;", "if a { } else b := 1;", "while a x foo;", "for i in items return;", "loop break;",
                           "case c of { 1: x; }"};
    for (const char* source : cases) {
        std::string diagnostics;
        dumpFirstItem(source, &diagnostics);
        EXPECT_NE(diagnostics.find("parse.expected"), std::string::npos) << source << "\n" << diagnostics;
    }
    std::string diagnostics;
    dumpFirstItem("if a > b return 1;", &diagnostics);
    EXPECT_NE(diagnostics.find("parse.expectedBraces"), std::string::npos) << diagnostics;
}

TEST(Parser, BlockInConditionMustBeParenthesised)
{
    std::string diagnostics;
    dumpFirstItem("if items detect: { :e | e } {\n}", &diagnostics);
    EXPECT_NE(diagnostics.find("parse.blockInCondition"), std::string::npos) << diagnostics;
    EXPECT_EQ(expectClean("if (items anySatisfy: { :e | e }) {\n}").substr(0, 3), "If\n");
}

TEST(Parser, ControlWordsAreReserved)
{
    std::string diagnostics;
    dumpFirstItem("in := 3;", &diagnostics);
    EXPECT_NE(diagnostics.find("parse.expected"), std::string::npos) << diagnostics;
}

TEST(Parser, TypeMessagesInTypePositions)
{
    EXPECT_EQ(expectClean("sector: array of: uint8 by: [0 to: 511];"),
              "VarDecl name=sector\n"
              "  type: TypeMessage selector=of:by:\n"
              "    receiver: Type name=array\n"
              "    argument: Type name=uint8\n"
              "    argument: TypeMessage selector=to:\n"
              "      receiver: Int value=0\n"
              "      argument: Int value=511\n");
    EXPECT_EQ(expectClean("w: Day mon to: Day fri;"),
              "VarDecl name=w\n"
              "  type: TypeMessage selector=to:\n"
              "    receiver: Unary selector=mon\n"
              "      receiver: Ident name=Day\n"
              "    argument: Unary selector=fri\n"
              "      receiver: Ident name=Day\n");
}

TEST(Parser, ConstantAloneIsNotAType)
{
    std::string diagnostics;
    dumpFirstItem("x: 5;", &diagnostics);
    EXPECT_NE(diagnostics.find("parse.expectedType"), std::string::npos) << diagnostics;
}

TEST(Parser, TupleAndGroupTypes)
{
    auto parsed = parse("{ class C { msg a [bool] { } msg b [int32, string] { } msg c [count: int32, name: string] { } } }");
    EXPECT_EQ(parsed->diagnosticsText(), "");
    std::string dump = bob::ast::dumpAstText(parsed->unit.unit);
    EXPECT_NE(dump.find("selector=a kind=unary\n      returns: Type name=bool"), std::string::npos) << dump;
    EXPECT_NE(dump.find("returns: Tuple\n        element: TupleElement\n          type: Type name=int32"), std::string::npos) << dump;
    EXPECT_NE(dump.find("element: TupleElement name=count"), std::string::npos) << dump;
}

TEST(Parser, MessageDeclarationForms)
{
    auto parsed = parse("{ class C {\n"
                        "  msg increment { }\n"
                        "  msg + other:Point [Point] { }\n"
                        "  msg toString : t:int32 isFat:bool [string] { }\n"
                        "  msg at: i:int64 put: value:int32 [int32] { }\n"
                        "  msg sum: nums:[array of: int32] [int64] { }\n"
                        "  msg area [real64];\n"
                        "} }");
    EXPECT_EQ(parsed->diagnosticsText(), "");
    std::string dump = bob::ast::dumpAstText(parsed->unit.unit);
    EXPECT_NE(dump.find("Message selector=increment kind=unary"), std::string::npos) << dump;
    EXPECT_NE(dump.find("Message selector=+ kind=binary\n      param: Param name=other"), std::string::npos) << dump;
    EXPECT_NE(dump.find("Message selector=toString:isFat: kind=keyword\n      param: Param label=toString name=t"), std::string::npos) << dump;
    EXPECT_NE(dump.find("param: Param name=isFat\n        type: Type name=bool"), std::string::npos) << dump;
    EXPECT_NE(dump.find("Message selector=at:put:"), std::string::npos) << dump;
    EXPECT_NE(dump.find("param: Param label=put name=value"), std::string::npos) << dump;
    EXPECT_NE(dump.find("param: Param label=sum name=nums\n        type: TypeMessage selector=of:"), std::string::npos) << dump;
    EXPECT_NE(dump.find("Message selector=area kind=unary required=true"), std::string::npos) << dump;
}

TEST(Parser, DuplicateParameterName)
{
    auto parsed = parse("{ class C { msg a: x:int32 b: x:int32 { } } }");
    EXPECT_NE(parsed->diagnosticsText().find("parse.duplicateParameter"), std::string::npos);
}

TEST(Parser, EnumsAndTypeAliases)
{
    auto parsed = parse("{ enum Color { red; green := 5; blue; }\n"
                        "enum PciClass: uint8 { storage := 0x01; }\n"
                        "type Port : uint16 := 1 to: 65535;\n"
                        "@[alias] type Handle := uint64; }");
    EXPECT_EQ(parsed->diagnosticsText(), "");
    std::string dump = bob::ast::dumpAstText(parsed->unit.unit);
    EXPECT_NE(dump.find("Enum name=Color\n    member: EnumMember name=red\n    member: EnumMember name=green\n      value: Int value=5"),
              std::string::npos)
        << dump;
    EXPECT_NE(dump.find("Enum name=PciClass\n    storage: Type name=uint8"), std::string::npos) << dump;
    EXPECT_NE(dump.find("TypeAlias name=Port\n    storage: Type name=uint16\n    type: TypeMessage selector=to:"), std::string::npos) << dump;
    EXPECT_NE(dump.find("TypeAlias name=Handle\n    attribute: Attribute name=alias"), std::string::npos) << dump;
}

TEST(Parser, UnitHeader)
{
    auto parsed = parse("@[overflow: wrap] module kernel.mm, 0.1.0\nimport core.io;\nimport core.x, 2.0;\n{ }");
    EXPECT_EQ(parsed->diagnosticsText(), "");
    EXPECT_EQ(bob::ast::dumpAstText(parsed->unit.unit),
              "Unit kind=module name=kernel.mm version=0.1.0\n"
              "  attribute: Attribute name=overflow\n"
              "    value: Ident name=wrap\n"
              "  import: Import name=core.io\n"
              "  import: Import name=core.x version=2.0\n");
}

TEST(Parser, MissingSemicolonIsReportedButParsingContinues)
{
    auto parsed = parse("{ a := 1\nb := 2; }");
    std::string diagnostics = parsed->diagnosticsText();
    EXPECT_NE(diagnostics.find("parse.expectedSemicolon"), std::string::npos) << diagnostics;
}

TEST(Parser, RecoversAfterABadStatement)
{
    auto parsed = parse("{ a := ;\nb := 2;\nclass C { msg 5 { } msg ok { } }\nc := 3; }");
    EXPECT_GE(parsed->diagnostics.errorCount(), 2u);
    const auto& items = parsed->unit.unit->items;
    ASSERT_EQ(items.size(), 4u);
    EXPECT_EQ(items[0]->kind, bob::ast::NodeKind::error);
    EXPECT_EQ(items[1]->kind, bob::ast::NodeKind::assignment);
    EXPECT_EQ(items[2]->kind, bob::ast::NodeKind::classDecl);
    EXPECT_EQ(items[3]->kind, bob::ast::NodeKind::assignment);
    auto classDecl = static_cast<bob::ast::ClassDecl*>(items[2]);
    ASSERT_EQ(classDecl->members.size(), 2u);
    EXPECT_EQ(classDecl->members[0]->kind, bob::ast::NodeKind::error);
    EXPECT_EQ(classDecl->members[1]->kind, bob::ast::NodeKind::messageDecl);
}

TEST(Parser, MessageOutsideClassIsAnError)
{
    auto parsed = parse("{ msg foo { } }");
    EXPECT_NE(parsed->diagnosticsText().find("parse.messageOutsideClass"), std::string::npos);
}

TEST(Parser, InvalidAssignmentTarget)
{
    auto parsed = parse("{ a + b := 3; }");
    EXPECT_NE(parsed->diagnosticsText().find("parse.invalidAssignmentTarget"), std::string::npos);
}

TEST(Parser, TrailingTokensAfterUnit)
{
    auto parsed = parse("{ } x");
    EXPECT_NE(parsed->diagnosticsText().find("parse.trailingTokens"), std::string::npos);
}

TEST(Parser, EmptyInputReportsMissingBody)
{
    auto parsed = parse("");
    EXPECT_NE(parsed->diagnosticsText().find("parse.expected"), std::string::npos);
    ASSERT_NE(parsed->unit.unit, nullptr);
}
