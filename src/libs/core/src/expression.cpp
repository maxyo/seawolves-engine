#include "compiler.h"

char ERR_INVALID_EXPRESSION[] = "Invalid Expression";


bool COMPILER::CompileExpression(SEGMENT_DESC &Segment)
{
    uint32_t dwVarCode;
    const VarInfo *real_var;
    LocalVarInfo lvi;

    const S_TOKEN_TYPE Token_type = CompileAuxiliaryTokens(Segment);
    if (Token_type == SEPARATOR)
    {
        SetError(ERR_INVALID_EXPRESSION);
        return false;
    }

    if (Token_type == AND)
    {

        S_TOKEN_TYPE sttVariableType = Token.Get();

        if (sttVariableType != UNKNOWN)
        {
            SetError("invalid '&' usage");
            return false;
        }

        sttVariableType = DetectUnknown(dwVarCode);
        if (!(sttVariableType == VARIABLE || sttVariableType == LOCAL_VARIABLE))
        {
            SetError("variable not found");
            return false;
        }

        const S_TOKEN_TYPE sttTokenType = Token.Get();

        // alloc for result
        CompileToken(Segment, STACK_ALLOC);

        if (sttTokenType == SQUARE_OPEN_BRACKET)
        {
            if (sttVariableType == VARIABLE)
            {
                real_var = VarTab.GetVarX(dwVarCode);
                if (real_var == nullptr)
                {
                    SetError("Invalid var code");
                    return false;
                }

                // check for possibilities of '[' operator
                if (!real_var->IsArray())
                {
                    if (real_var->type != VAR_REFERENCE)
                    {
                        SetError("EN: %d", real_var->elements);
                        SetError(" A Invalid '[' operator, %s - isnt array", real_var->name.c_str());
                        return false;
                    }
                }
            }
            else
            {
                FuncTab.GetVar(lvi, CurrentFuncCode, dwVarCode);
                // check for possibilities of '[' operator
                if (!lvi.IsArray())
                {
                    if (lvi.type != VAR_REFERENCE)
                    {
                        SetError(" B Invalid '[' operator, %s - isnt array", lvi.name.c_str());
                        return false;
                    }
                }
            }

            // alloc for array index
            // after top of the stack must be array index
            Token.Get();
            if (!CompileExpression_L2(Segment))
            {
                SetError("invalid syntax");
                return false;
            }
            // test for ']'
            if (Token.GetType() != SQUARE_CLOSE_BRACKET)
            {
                SetError("missing ']'");
                return false;
            }

            CompileToken(Segment, STACK_POP);
            CompileToken(Segment, BX);

            CompileToken(Segment, SETREF_BXINDEX);
            CompileToken(Segment, STACK_TOP);
            CompileToken(Segment, sttVariableType, 1, &dwVarCode, sizeof(uint32_t));

            // next after ']'
            CompileAuxiliaryTokens(Segment);
        }
        else
        {
            CompileToken(Segment, SETREF);
            CompileToken(Segment, STACK_TOP);
            CompileToken(Segment, sttVariableType, 1, &dwVarCode, sizeof(uint32_t));
        }


        return true;
    }
    return CompileExpression_L0(Segment);
}

// logical operators (OR,AND)
bool COMPILER::CompileExpression_L0(SEGMENT_DESC &Segment)
{
    uint32_t dwJumpOffset1;
    uint32_t dwJumpOffset2;

    if (!CompileExpression_L1(Segment))
    {
        SetError(ERR_INVALID_EXPRESSION);
        return false;
    }

    S_TOKEN_TYPE sttOpType = Token.GetType();

    while (sttOpType == OP_BOOL_AND || sttOpType == OP_BOOL_OR)
    {
        sttOpType = Token.GetType();

        CompileAuxiliaryTokens(Segment);

        // copy to EX value of first operand

        CompileToken(Segment, STACK_READ);
        CompileToken(Segment, EX);

        const uint32_t dwUpdateOffset1 = Segment.BCode_Program_size + 2;
        // point of jump not determined yet
        dwJumpOffset1 = 0;
        // compile jump command
        if (sttOpType == OP_BOOL_AND)
        {
            // AND : if 1st operand is zero - doesnt need to calculate second - jump to jump1
            CompileToken(Segment, JUMP_Z, 1, (char *)&dwJumpOffset1, sizeof(uint32_t));
        }
        else
        {
            // OR : if 1st operand is non zero - doesnt need to calculate second - jump to jump1
            CompileToken(Segment, JUMP_NZ, 1, (char *)&dwJumpOffset1, sizeof(uint32_t));
        }

        if (!CompileExpression_L1(Segment))
        {
            SetError(ERR_INVALID_EXPRESSION);
            return false;
        }

        CompileToken(Segment, STACK_POP);
        CompileToken(Segment, AX);

        CompileToken(Segment, sttOpType);
        CompileToken(Segment, STACK_TOP);
        CompileToken(Segment, AX);

        // skip next section - proceed to jump2
        const uint32_t dwUpdateOffset2 = Segment.BCode_Program_size + 2;
        dwJumpOffset2 = 0;
        CompileToken(Segment, JUMP, 1, (char *)&dwJumpOffset2, sizeof(uint32_t));

        // jump1
        // just convert stack top to bool
        memcpy(&Segment.pCode[dwUpdateOffset1], &Segment.BCode_Program_size, sizeof(uint32_t));
        CompileToken(Segment, OP_BOOL_CONVERT);
        CompileToken(Segment, STACK_TOP);

        // jump2
        // end of operation
        memcpy(&Segment.pCode[dwUpdateOffset2], &Segment.BCode_Program_size, sizeof(uint32_t));

        sttOpType = Token.GetType();
    }
    return true;
}

// logical compare (>,<,==,>=,<=,!=)
bool COMPILER::CompileExpression_L1(SEGMENT_DESC &Segment)
{
    if (!CompileExpression_L2(Segment))
    {
        SetError(ERR_INVALID_EXPRESSION);
        return false;
    }

    S_TOKEN_TYPE sttOpType = Token.GetType();

    while (sttOpType == OP_NOT_EQUAL || sttOpType == OP_LESSER_OR_EQUAL || sttOpType == OP_LESSER ||
           sttOpType == OP_GREATER_OR_EQUAL || sttOpType == OP_GREATER || sttOpType == OP_BOOL_EQUAL)
    {
        sttOpType = Token.GetType();

        CompileAuxiliaryTokens(Segment);

        if (!CompileExpression_L2(Segment))
        {
            SetError(ERR_INVALID_EXPRESSION);
            return false;
        }

        CompileToken(Segment, STACK_POP);
        CompileToken(Segment, AX);
        CompileToken(Segment, sttOpType);
        CompileToken(Segment, STACK_TOP);
        CompileToken(Segment, AX);

        sttOpType = Token.GetType();
    }

    return true;
}

// '+' and '-' operators
bool COMPILER::CompileExpression_L2(SEGMENT_DESC &Segment)
{
    if (!CompileExpression_L3(Segment))
    {
        SetError(ERR_INVALID_EXPRESSION);
        return false;
    }

    S_TOKEN_TYPE sttOpType = Token.GetType();

    while (sttOpType == OP_PLUS || sttOpType == OP_MINUS)
    {
        sttOpType = Token.GetType();

        CompileAuxiliaryTokens(Segment);

        if (!CompileExpression_L3(Segment))
        {
            SetError(ERR_INVALID_EXPRESSION);
            return false;
        }

        CompileToken(Segment, STACK_POP);
        CompileToken(Segment, AX);
        CompileToken(Segment, sttOpType);
        CompileToken(Segment, STACK_TOP);
        CompileToken(Segment, AX);

        sttOpType = Token.GetType();
    }
    return true;
}

// '*' and '/' operators
bool COMPILER::CompileExpression_L3(SEGMENT_DESC &Segment)
{
    if (!CompileExpression_L4(Segment))
    {
        SetError(ERR_INVALID_EXPRESSION);
        return false;
    }

    S_TOKEN_TYPE sttOpType = Token.GetType();

    while (sttOpType == OP_MULTIPLY || sttOpType == OP_DIVIDE || sttOpType == OP_MODUL)
    {
        sttOpType = Token.GetType();
        CompileAuxiliaryTokens(Segment);

        if (!CompileExpression_L4(Segment))
        {
            SetError(ERR_INVALID_EXPRESSION);
            return false;
        }

        CompileToken(Segment, STACK_POP);
        CompileToken(Segment, AX);
        CompileToken(Segment, sttOpType);
        CompileToken(Segment, STACK_TOP);
        CompileToken(Segment, AX);

        sttOpType = Token.GetType();
    }
    return true;
}

// '^' operations
bool COMPILER::CompileExpression_L4(SEGMENT_DESC &Segment)
{
    if (!CompileExpression_L5(Segment))
    {
        SetError(ERR_INVALID_EXPRESSION);
        return false;
    }

    if (Token.GetType() == OP_POWER)
    {
        CompileAuxiliaryTokens(Segment);

        if (!CompileExpression_L5(Segment))
        {
            SetError(ERR_INVALID_EXPRESSION);
            return false;
        }

        CompileToken(Segment, STACK_POP);
        CompileToken(Segment, AX);
        CompileToken(Segment, OP_POWER);
        CompileToken(Segment, STACK_TOP);
        CompileToken(Segment, AX);
    }
    return true;
}

// sign
bool COMPILER::CompileExpression_L5(SEGMENT_DESC &Segment)
{
    const S_TOKEN_TYPE sttOpType = Token.GetType();
    if (sttOpType == OP_PLUS || sttOpType == OP_MINUS || sttOpType == OP_BOOL_NEG)
        CompileAuxiliaryTokens(Segment);

    if (!CompileExpression_L6(Segment))
    {
        SetError(ERR_INVALID_EXPRESSION);
        return false;
    }

    switch (sttOpType)
    {
    case OP_MINUS:
        CompileToken(Segment, OP_SMINUS);
        CompileToken(Segment, STACK_TOP);
        break;
    case OP_BOOL_NEG:
        CompileToken(Segment, OP_BOOL_NEG);
        CompileToken(Segment, STACK_TOP);
        break;
    }
    return true;
}

// '(' and ')'
bool COMPILER::CompileExpression_L6(SEGMENT_DESC &Segment)
{
    if (Token.GetType() == OPEN_BRACKET)
    {
        CompileAuxiliaryTokens(Segment);
        if (!CompileExpression_L1(Segment))
        {
            SetError(ERR_INVALID_EXPRESSION);
            return false;
        }
        if (Token.GetType() != CLOSE_BRACKET)
        {
            SetError("No matching ')' in expression");
            return false;
        }
        CompileAuxiliaryTokens(Segment);
        return true;
    }
    return CompileExpression_L7(Segment);
}

bool COMPILER::CompileExpression_L7(SEGMENT_DESC &Segment)
{
    uint32_t dwRCode = 0;
    S_TOKEN_TYPE sttResult;
    S_TOKEN_TYPE sttVariableField;
    S_TOKEN_TYPE sttFuncCallType;
    uint32_t dwVarCode;
    uint32_t dwAWCode;
    const VarInfo *real_var;
    LocalVarInfo lvi;
    FuncInfo fi;

    bool bDynamicCall = false;
    switch (Token.GetType())
    {
    case SEPARATOR:

        return true;
    case NUMBER:
        CompileToken(Segment, STACK_PUSH);
        CompileNumber(Segment);
        CompileAuxiliaryTokens(Segment);
        return true;
    case FLOAT_NUMBER:
        CompileToken(Segment, STACK_PUSH);
        CompileFloatNumber(Segment);
        CompileAuxiliaryTokens(Segment);
        return true;
    case STRING:
        CompileToken(Segment, STACK_PUSH);
        CompileString(Segment);
        CompileAuxiliaryTokens(Segment);
        return true;
    case TRUE_CONST:
        CompileToken(Segment, STACK_PUSH);
        dwRCode = 1;
        CompileToken(Segment, NUMBER, 1, (char *)&dwRCode, sizeof(uint32_t));
        CompileAuxiliaryTokens(Segment);
        return true;
    case FALSE_CONST:
        CompileToken(Segment, STACK_PUSH);
        dwRCode = 0;
        CompileToken(Segment, NUMBER, 1, (char *)&dwRCode, sizeof(uint32_t));
        CompileAuxiliaryTokens(Segment);
        return true;

    case CALL:
        bDynamicCall = true;
    case UNKNOWN:
        if (!bDynamicCall)
        {
            sttResult = DetectUnknown(dwRCode);
            // set func call method to standart
            sttFuncCallType = CALL_FUNCTION;
        }
        else
        {
            sttResult = CALL;
            sttFuncCallType = CALL_FUNCTION;
        }

        switch (sttResult)
        {
        case DEFINE_VAL:
            DEFINFO di;
            DefTab.GetDef(di, dwRCode);
            switch (di.deftype)
            {
            case NUMBER:
                CompileToken(Segment, STACK_PUSH);
                CompileToken(Segment, NUMBER, 1, (char *)&di.data4b, sizeof(uint32_t));
                CompileAuxiliaryTokens(Segment);
                break;
            case FLOAT_NUMBER:
                CompileToken(Segment, STACK_PUSH);
                CompileToken(Segment, FLOAT_NUMBER, 1, (char *)&di.data4b, sizeof(uint32_t));
                CompileAuxiliaryTokens(Segment);
                break;
            case STRING:
                CompileToken(Segment, STACK_PUSH);
                uint32_t string_size;
                string_size = strlen((char *)di.data4b) + 1;
                CompileToken(Segment, STRING, 2, (char *)&string_size, sizeof(uint32_t), (char *)di.data4b,
                             string_size);
                CompileAuxiliaryTokens(Segment);
                break;
            }
            return true;
        case CALL:
            // this is dynamic function call

            sttFuncCallType = CALL;

            if (Token.Get() != UNKNOWN)
            {
                SetError("invalid dynamic call syntax");
                return false;
            }

            sttVariableField = DetectUnknown(dwVarCode);

            if (sttVariableField == VARIABLE)
            {
                real_var = VarTab.GetVarX(dwVarCode);
                if (real_var == nullptr)
                {
                    SetError("Invalid var code");
                    return false;
                }

                if (real_var->type != VAR_STRING)
                {
                    SetError("'%s' must be string variable", Token.GetData());
                    return false;
                }
            }
            else if (sttVariableField == LOCAL_VARIABLE)
            {
                FuncTab.GetVar(lvi, CurrentFuncCode, dwVarCode);
                if (lvi.type != VAR_STRING)
                {
                    SetError("'%s' must be string variable", Token.GetData());
                    return false;
                }
            }
            else
            {
                SetError("invalid dynamic call syntax: '%s' must be string variable", Token.GetData());
                return false;
            }

            // proceed to standart func call routine

        case CALL_FUNCTION:
            // this is function call

            // first prepare stack for calling function

            if (Token.Get() != OPEN_BRACKET)
            {
                SetError("missing '('");
                return false;
            }

            // just get, already checked by DetectUnknown
            FuncTab.GetFuncX(fi, dwRCode);


            uint32_t func_args;
            func_args = 0;

            if (fi.arguments == 0 && !IsIntFuncVarArgsNum(dwRCode) && sttFuncCallType != CALL)
            {
                // function without parameters
                if (Token.Get() != CLOSE_BRACKET)
                {
                    SetError("missing ')'");
                    return false;
                }
            }
            else
            {
                do
                {
                    // compile expression leave in stack top expression result
                    if (!CompileExpression(Segment))
                    {
                        SetError("invalid function argument");
                        return false;
                    }
                    if (Token.GetType() != COMMA)
                    {
                        if (Token.GetType() != CLOSE_BRACKET)
                        {
                            SetError("invalid function argument");
                            return false;
                        }
                    }
                    func_args++;
                } while (Token.GetType() == COMMA);
            }


            if (sttFuncCallType == CALL_FUNCTION)
            {
                CheckCallArguments(fi, dwRCode, func_args);
                CompileToken(Segment, CALL_FUNCTION, 1, (char *)&dwRCode, sizeof(uint32_t));
            }
            else
            {
                // code of function must be find out on the fly from variable
                CompileToken(Segment, CALL);
                CompileToken(Segment, sttVariableField, 1, (char *)&dwVarCode, sizeof(uint32_t));
            }
            CompileToken(Segment, ARGS_NUM, 1, (char *)&func_args, sizeof(uint32_t));

            CompileAuxiliaryTokens(Segment);

            return true;
        case LOCAL_VARIABLE:
        case VARIABLE:

            sttVariableField = sttResult;

            // variable code
            dwVarCode = dwRCode;

            // we going to place into stack variable value or attribute string - alloc stack data
            CompileToken(Segment, STACK_ALLOC);

            sttResult = Token.Get();
            if (sttResult == SQUARE_OPEN_BRACKET)
            {
                // way for array element

                // doesnt need to check, becose already pass DetectUnknown function

                if (sttVariableField == VARIABLE)
                {
                    real_var = VarTab.GetVarX(dwVarCode);
                    if (real_var == nullptr)
                    {
                        SetError("Invalid var code");
                        return false;
                    }

                    // check for possibilities of '[' operator
                    if (!real_var->IsArray())
                    {
                        if (real_var->type != VAR_REFERENCE)
                        {
                            SetError(" C Invalid '[' operator, %s - isnt array", real_var->name.c_str());
                            return false;
                        }
                    }
                }
                else
                {
                    FuncTab.GetVar(lvi, CurrentFuncCode, dwVarCode);
                    // check for possibilities of '[' operator
                    if (!lvi.IsArray())
                    {
                        if (lvi.type != VAR_REFERENCE)
                        {
                            SetError(" D Invalid '[' operator, %s - isnt array", lvi.name.c_str());
                            return false;
                        }
                    }
                }

                // after top of the stack must be array index
                Token.Get();
                if (!CompileExpression_L2(Segment))
                    return false;
                // test for ']'
                if (Token.GetType() != SQUARE_CLOSE_BRACKET)
                {
                    SetError("missing ']'");
                    return false;
                }
                sttResult = CompileAuxiliaryTokens(Segment);

                // if not accessing attribute - copy value and return
                if (sttResult != DOT)
                {
                    // optimiztion possible: MOVE [STACK-1],[STACK]

                    CompileToken(Segment, STACK_POP);
                    CompileToken(Segment, BX);

                    // write to stack element value (index in X)
                    CompileToken(Segment, STACK_WRITE_BXINDEX);
                    CompileToken(Segment, sttVariableField, 1, (char *)&dwVarCode, sizeof(uint32_t));


                    return true;
                }

                // in other case want to access attribute string value

                CompileToken(Segment, STACK_POP);
                CompileToken(Segment, BX);

                // move ap to AP register
                CompileToken(Segment, MOVEAP_BXINDEX);
                CompileToken(Segment, AP);
                CompileToken(Segment, sttVariableField, 1, (char *)&dwVarCode, sizeof(uint32_t));

            }
            else
            {
                // way for non array var

                if (sttResult != DOT)
                {
                    CompileToken(Segment, STACK_WRITE);
                    CompileToken(Segment, sttVariableField, 1, (char *)&dwVarCode, sizeof(uint32_t));
                    return true;
                }

                // in other case want to access attribute string value

                // move ap to AP register
                CompileToken(Segment, MOVEAP);
                CompileToken(Segment, AP);
                CompileToken(Segment, sttVariableField, 1, (char *)&dwVarCode, sizeof(uint32_t));
            }

            // ADVANCE AP

            sttResult = Token.Get();
            do
            {
                if (Token.GetType() == OPEN_BRACKET)
                {
                    // access var
                    sttResult = Token.Get();
                    if (sttResult != UNKNOWN)
                    {
                        SetError("Invalid access var syntax");
                        return false;
                    }
                    sttResult = DetectUnknown(dwVarCode);
                    if (!(sttResult == VARIABLE || sttResult == LOCAL_VARIABLE))
                    {
                        SetError("not variable: %s", Token.GetData());
                        return false;
                    }
                    CompileToken(Segment, ADVANCE_AP);
                    CompileToken(Segment, sttResult, 1, &dwVarCode, sizeof(uint32_t));
                    if (Token.Get() != CLOSE_BRACKET)
                    {
                        SetError("missing ')'");
                        return false;
                    }
                }
                else
                {
                    // access word
                    if (Token.GetData() == nullptr)
                    {
                        SetError("Invalid access string");
                        return false;
                    }
                    Token.LowCase();
                    dwAWCode = SCodec.Convert(Token.GetData());
                    CompileToken(Segment, ADVANCE_AP);
                    CompileToken(Segment, ACCESS_WORD_CODE, 1, &dwAWCode, sizeof(uint32_t));
                }
                sttResult = Token.Get();
                if (sttResult == DOT)
                    Token.Get(); // get next token (after dot)
            } while (sttResult == DOT);

            // write attribute string to stack top
            CompileToken(Segment, STACK_WRITE);
            CompileToken(Segment, AP_VALUE);

            return true;
        default:
            // ???
            SetError(ERR_INVALID_EXPRESSION);
            return false;
        }
        return true;
    }
    SetError(ERR_INVALID_EXPRESSION);
    return false;
}
