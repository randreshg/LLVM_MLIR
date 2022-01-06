#include <string>
#include <iostream>
#include <vector>

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
/*
    Each token returned by our lexer will either be one of the Token enum values or it will be 
    an ‘unknown’ character like ‘+’, which is returned as its ASCII value. If the current token 
    is an identifier, the IdentifierStr global variable holds the name of the identifier. 
    If the current token is a numeric literal (like 1.0), NumVal holds its value.
    
    The actual implementation of the lexer is a single function named gettok. 
    The gettok function is called to return the next token from standard input.
*/
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
// ExprAST - Base class for all expression nodes.
class ExprAST {
public:
    virtual ~ExprAST() {}
};

// NumberExprAST - Expression class for numeric literals like "1.0".
class NumberExprAST : public ExprAST {
    double Val;
public:
    NumberExprAST(double val) : Val(val) {}
};

// VariableExprAST - Expression class for referencing a variable, like "a".
class VariableExprAST : public ExprAST {
    std::string Name;
public:
    VariableExprAST(const std::string &name) : Name(name) {}
};

// BinaryExprAST - Expression class for a binary operator.
class BinaryExprAST : public ExprAST {
    char Op; // + - * / > <
    ExprAST *LHS, *RHS;
public:
    BinaryExprAST(char op, ExprAST *lhs, ExprAST *rhs)
        : Op(op), LHS(lhs), RHS(rhs) {}
};

// CallExprAST - Expression class for function calls.
class CallExprAST : public ExprAST {
    std::string Callee;
    std::vector<ExprAST*> Args;
public:
    CallExprAST(const std::string &callee, std::vector<ExprAST*> &args)
        : Callee(callee), Args(args) {}
};

// PrototypeAST - This class represents the "prototype" for a function,
// which captures its name, and its argument names (thus implicitly the number
// of arguments the function takes).
class PrototypeAST {
    std::string Name;
    std::vector<std::string> Args;
public:
    PrototypeAST(const std::string &name, const std::vector<std::string> &args)
        : Name(name), Args(args) {}
};

// FunctionAST - This class represents a function definition itself.
class FunctionAST {
    PrototypeAST *Proto;
    ExprAST *Body;
    public:
        FunctionAST(PrototypeAST *proto, ExprAST *body)
            : Proto(proto), Body(body) {}
};

//===----------------------------------------------------------------------===//
/*  PARSER
    The idea here is that we want to parse something like “x+y” (which is 
    returned as three tokens by the lexer) into an AST that could be generated 
    with calls
*/
//===----------------------------------------------------------------------===//
/*  CurTok/getNextToken - Provide a simple token buffer.  CurTok is the current
    token the parser is looking at.  getNextToken reads another token from the
    lexer and updates CurTok with its results.
*/
static int CurTok;
static int getNextToken() {
    return CurTok = gettok();
}
/// Error* - These are little helper functions for error handling.
ExprAST *Error(const char *Str) { fprintf(stderr, "Error: %s\n", Str);return 0;}
PrototypeAST *ErrorP(const char *Str) { Error(Str); return 0; }
FunctionAST *ErrorF(const char *Str) { Error(Str); return 0; }

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

// numberexpr ::= number
static ExprAST *ParseNumberExpr() {
    ExprAST *Result = new NumberExprAST(NumVal);
    getNextToken(); // consume the number
    return Result;
}

// parenexpr ::= '(' expression ')'
static ExprAST *ParseParenExpr() {
    getNextToken();  // eat (.
    ExprAST *V = ParseExpression();
    if (!V) return 0;
    if (CurTok != ')') return Error("expected ')'");
    getNextToken();  // eat ).
    return V;
}

// identifierexpr
//   ::= identifier
//   ::= identifier '(' expression* ')'
static ExprAST *ParseIdentifierOrCallExpr() {
    std::string IdName = IdentifierStr;
    getNextToken();  // eat identifier.
    if (CurTok != '(') // Simple variable ref.
        return new VariableExprAST(IdName);
    // Call.
    getNextToken();  // eat (
    std::vector<ExprAST*> Args;
    if (CurTok != ')') {
        while (1) {
            ExprAST *Arg = ParseExpression();
            if (!Arg) return 0;
            Args.push_back(Arg);

            if (CurTok == ')') break;
            if (CurTok != ',')
                return Error("Expected ')' or ',' in argument list");
            getNextToken();
        }
    }
    getNextToken(); // Eat the ')'.
    return new CallExprAST(IdName, Args);
}

// primary
//   ::= identifierexpr
//   ::= numberexpr
//   ::= parenexpr
static ExprAST *ParsePrimary() {
    switch (CurTok) {
        case tok_identifier: return ParseIdentifierOrCallExpr();
        case tok_number:     return ParseNumberExpr();
        case '(':            return ParseParenExpr();
        default: return Error("unknown token when expecting an expression");
    }
}

// binoprhs
//   ::= ('+' primary)*
static ExprAST *ParseBinOpRHS(int ExprPrec, ExprAST *LHS) {
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
        ExprAST *RHS = ParsePrimary();
        if (!RHS) return 0;
        // If BinOp binds less tightly with RHS than the operator after RHS, let
        // the pending operator take RHS as its LHS.
        int NextPrec = GetTokPrecedence();
        if (TokPrec < NextPrec) {
            RHS = ParseBinOpRHS(TokPrec+1, RHS);
            if (RHS == 0) return 0;
        }
        // Merge LHS/RHS.
        LHS = new BinaryExprAST(BinOp, LHS, RHS);
    }
}

// expression
//   ::= primary binoprhs
//
static ExprAST *ParseExpression() {
    ExprAST *LHS = ParsePrimary();
    if (!LHS) return 0;

    return ParseBinOpRHS(0, LHS);
}


int main () {
    // while (true) {
    //     int tok = gettok();
    //     std::cout<< "go token: " << tok << std::endl;
    // }
}