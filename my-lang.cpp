#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace llvm;

//===----------------------------------------------------------------------===//
/*  LEXER
    It breaks the input up into “tokens”. Each token returned by the lexer includes 
    a token code and potentially some metadata (e.g. the numeric value of a number)
    The lexer returns tokens [0-255] if it is an unknown character, otherwise one
    of these for known things.
*/
//===----------------------------------------------------------------------===//
enum Token {
    tok_eof = -1,
    // commands
    tok_def = -2, tok_extern = -3,
    // primary
    tok_identifier = -4, tok_number = -5,
};
static std::string IdentifierStr;  // Filled in if tok_identifier
static double NumVal;              // Filled in if tok_number

// gettok - Return the next token from standard input.
static int gettok() {
    static int LastChar = ' ';
    // Skip any whitespace.
    while (isspace(LastChar))
        LastChar = getchar();
    // Recognize identifiers and specific keywords
    if (isalpha(LastChar)) { // Identifier: [a-zA-Z][a-zA-Z0-9]*
        IdentifierStr = LastChar;
        while (isalnum((LastChar = getchar())))
            IdentifierStr += LastChar;
        if (IdentifierStr == "def") return tok_def;
        if (IdentifierStr == "extern") return tok_extern;
        return tok_identifier;
     }
    // Recognize numeric values
    if (isdigit(LastChar) || LastChar == '.') {   // Number: [0-9.]+
        std::string NumStr;
        do {
            NumStr += LastChar;
            LastChar = getchar();
        } while (isdigit(LastChar) || LastChar == '.');

        NumVal = strtod(NumStr.c_str(), 0);
        return tok_number;
    }
    // Recognize comments
    if (LastChar == '#') {
        // Comment until end of line.
        do {
            LastChar = getchar();
        }
        while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');

        if (LastChar != EOF)
            return gettok();
    }
    // Check for end of file.
    if (LastChar == EOF)
        return tok_eof;
    // Otherwise, just return the character as its ascii value.
    int ThisChar = LastChar;
    LastChar = getchar();
    return ThisChar;
}

//===----------------------------------------------------------------------===//
/*  ABSTRACT SYNTAX TREE
    The AST for a program captures its behavior in such a way that it is easy for 
    later stages of the compiler (e.g. code generation) to interpret. 
*/
//===----------------------------------------------------------------------===//
namespace {
// ExprAST - Base class for all expression nodes.
class ExprAST {
public:
    virtual ~ExprAST() = default;
    // The Codegen() method says to emit IR for that AST node along with 
    // all the things it depends on, and they all return an LLVM Value object
    virtual Value *codegen() = 0;
};

// NumberExprAST - Expression class for numeric literals like "1.0".
class NumberExprAST : public ExprAST {
    double Val;
public:
    NumberExprAST(double val) : Val(val) {}
    Value *codegen() override;
};

// VariableExprAST - Expression class for referencing a variable, like "a".
class VariableExprAST : public ExprAST {
    std::string Name;
public:
    VariableExprAST(const std::string &name) : Name(name) {}
    Value *codegen() override;
};

// BinaryExprAST - Expression class for a binary operator.
class BinaryExprAST : public ExprAST {
    char Op; // + - * / > <
    std::unique_ptr<ExprAST> LHS, RHS;
public:
    BinaryExprAST(char op, std::unique_ptr<ExprAST> lhs, std::unique_ptr<ExprAST> rhs)
        : Op(op), LHS(std::move(lhs)), RHS(std::move(rhs)) {}
    Value *codegen() override;
};

// CallExprAST - Expression class for function calls.
class CallExprAST : public ExprAST {
    std::string Callee;
    std::vector<std::unique_ptr<ExprAST>> Args;
public:
    CallExprAST(const std::string &callee, std::vector<std::unique_ptr<ExprAST>> &args)
        : Callee(callee), Args(std::move(Args)) {}
    Value *codegen() override;
};

// PrototypeAST - This class represents the "prototype" for a function,
// which captures its name, and its argument names (thus implicitly the number
// of arguments the function takes).
class PrototypeAST {
    std::string Name;
    std::vector<std::string> Args;
public:
    PrototypeAST(const std::string &name, const std::vector<std::string> args)
        : Name(name), Args(args) {}
    Function *codegen();
    const std::string &getName() const { return Name; }
};

// FunctionAST - This class represents a function definition itself.
class FunctionAST {
    std::unique_ptr<PrototypeAST> Proto;
    std::unique_ptr<ExprAST> Body;
public:
    FunctionAST(std::unique_ptr<PrototypeAST> proto, std::unique_ptr<ExprAST> body)
        : Proto(std::move(proto)), Body(std::move(body)) {}
    Function *codegen();
};
}

//===----------------------------------------------------------------------===//
/*  PARSER
    The idea here is that we want to parse something like “x+y” (which is 
    returned as three tokens by the lexer) into an AST that could be generated 
    with calls
*/
//===----------------------------------------------------------------------===//
//Declare function
static std::unique_ptr<ExprAST> ParseExpression();
/*  CurTok/getNextToken - Provide a simple token buffer.  CurTok is the current
    token the parser is looking at.  getNextToken reads another token from the
    lexer and updates CurTok with its results.
*/
static int CurTok;
static int getNextToken() { return CurTok = gettok(); }

// GetTokPrecedence - Get the precedence of the pending binary operator token.
static int GetTokPrecedence() {
    switch (CurTok) {
        case '>':
        case '<':
            return 10;
        case '+':
        case '-':
            return 20;
        case '*':
        case '/':
            return 40;
        default:
            return -1;
    }
}

// Error* - These are little helper functions for error handling.
std::unique_ptr<ExprAST> LogError(const char *str) {
    fprintf(stderr, "Error: %s\n", str);
    return nullptr;
}

std::unique_ptr<PrototypeAST> LogErrorP(const char *Str) {
    LogError(Str);
    return nullptr;
}

// numberexpr ::= number
static std::unique_ptr<ExprAST> ParseNumberExpr() {
    auto Result = std::make_unique<NumberExprAST>(NumVal);
    getNextToken(); // consume the number
    return std::move(Result);
}

// parenexpr ::= '(' expression ')'
static std::unique_ptr<ExprAST> ParseParenExpr() {
  getNextToken(); // eat (.
  auto V = ParseExpression();
  if (!V)
    return nullptr;

  if (CurTok != ')')
    return LogError("expected ')'");
  getNextToken(); // eat ).
  return V;
}

// identifierexpr
//   ::= identifier
//   ::= identifier '(' expression* ')'
static std::unique_ptr<ExprAST> ParseIdentifierOrCallExpr() {
    std::string IdName = IdentifierStr;
    getNextToken(); // eat identifier.
    if (CurTok != '(') // Simple variable ref.
        return std::make_unique<VariableExprAST>(IdName);
    // Call.
    getNextToken(); // eat (
    std::vector<std::unique_ptr<ExprAST>> Args;
    if (CurTok != ')') {
        while (true) {
            if (auto Arg = ParseExpression())
                Args.push_back(std::move(Arg));
            else
                return nullptr;
            if (CurTok == ')') break;
            if (CurTok != ',') return LogError("Expected ')' or ',' in argument list");
            getNextToken();
        }
    }
    getNextToken(); // Eat the ')'.
    return std::make_unique<CallExprAST>(IdName, std::move(Args));
}

// primary
//   ::= identifierexpr
//   ::= numberexpr
//   ::= parenexpr
static std::unique_ptr<ExprAST> ParsePrimary() {
    switch (CurTok) {
        case tok_identifier: return ParseIdentifierOrCallExpr();
        case tok_number:     return ParseNumberExpr();
        case '(':            return ParseParenExpr();
        default: return LogError("unknown token when expecting an expression");
    }
}

// binoprhs
//   ::= ('+' primary)*
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec, std::unique_ptr<ExprAST> LHS) {
    // If this is a binop, find its precedence.
    while (1) {
        int TokPrec = GetTokPrecedence();
        // If this is a binop that binds at least as tightly as the current binop,
        // consume it, otherwise we are done.
        if (TokPrec < ExprPrec) return LHS;
        // Okay, we know this is a binop.
        int BinOp = CurTok;
        getNextToken();  // eat binop
        // Parse the primary expression after the binary operator.
        auto RHS = ParsePrimary();
        if (!RHS) return nullptr;
        // If BinOp binds less tightly with RHS than the operator after RHS, let
        // the pending operator take RHS as its LHS.
        int NextPrec = GetTokPrecedence();
        if (TokPrec < NextPrec) {
            RHS = ParseBinOpRHS(TokPrec + 1, std::move(RHS));
            if (!RHS) return nullptr;
        }
        // Merge LHS/RHS.
        LHS = std::make_unique<BinaryExprAST>(BinOp, std::move(LHS), std::move(RHS));
    }
}

// expression
//   ::= primary binoprhs
//
static std::unique_ptr<ExprAST> ParseExpression() {
    auto LHS = ParsePrimary();
    if (!LHS) return nullptr;
    return ParseBinOpRHS(0, std::move(LHS));
}

// prototype
//   ::= id '(' id* ')'
static std::unique_ptr<PrototypeAST> ParsePrototype() {
    if (CurTok != tok_identifier)
        return LogErrorP("Expected function name in prototype");

    std::string FnName = IdentifierStr;
    getNextToken();
    if (CurTok != '(')
        return LogErrorP("Expected '(' in prototype");

    // Read the list of argument names.
    std::vector<std::string> ArgNames;
    while (getNextToken() == tok_identifier)
        ArgNames.push_back(IdentifierStr);
    if (CurTok != ')')
        return LogErrorP("Expected ')' in prototype");

    // success.
    getNextToken();  // eat ')'.

    return std::make_unique<PrototypeAST>(FnName, ArgNames);
}

// definition ::= 'def' prototype expression
static std::unique_ptr<FunctionAST> ParseDefinition() {
    getNextToken();  // eat def.
    auto Proto = ParsePrototype();
    if (!Proto) return nullptr;

    if (auto E = ParseExpression())
        return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
    return nullptr;
}

// toplevelexpr ::= expression
static std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
    if (auto E = ParseExpression()) {
        // Make an anonymous proto.
        auto Proto = std::make_unique<PrototypeAST>("__anon_expr",
                                                    std::vector<std::string>());
        return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
    }
    return nullptr;
}

// external ::= 'extern' prototype
static std::unique_ptr<PrototypeAST> ParseExtern() {
    getNextToken();  // eat extern.
    return ParsePrototype();
}

//===----------------------------------------------------------------------===//
// Top-Level parsing
//===----------------------------------------------------------------------===//
static void HandleDefinition() {
  if (ParseDefinition()) {
    fprintf(stderr, "Parsed a function definition.\n");
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

static void HandleExtern() {
  if (ParseExtern()) {
    fprintf(stderr, "Parsed an extern\n");
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

static void HandleTopLevelExpression() {
  // Evaluate a top-level expression into an anonymous function.
  if (ParseTopLevelExpr()) {
    fprintf(stderr, "Parsed a top-level expr\n");
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

/// top ::= definition | external | expression | ';'
static void MainLoop() {
    while (1) {
        fprintf(stderr, "ready> ");
        switch (CurTok) {
            case tok_eof:    return;
            case ';':        getNextToken(); break;  // ignore top-level semicolons.
            case tok_def:    HandleDefinition(); break;
            case tok_extern: HandleExtern(); break;
            default:         HandleTopLevelExpression(); break;
        }
    }
}

int main () {
    // while (true) {
    //     int tok = gettok();
    //     std::cout<< "go token: " << tok << std::endl;
    // }
    // Prime the first token.
    fprintf(stderr, "ready> ");
    getNextToken();

    // Run the main "interpreter loop" now.
    MainLoop();

    return 0;
}