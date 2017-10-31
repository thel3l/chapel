/*
 * Copyright 2004-2017 Cray Inc.
 * Other additional copyright holders may be indicated within.
 *
 * The entirety of this work is licensed under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//////////////////////////////////////////////////////////////////////////////
//
// Wrappers are used to lower the Chapel idea of a function call to something
// implementable in C.
//  default wrapper -- supplies a value for every argument in the called
//      function substituting default values for actual arguments that are
//       omitted.
//      (C does not support default values for arguments.)
//
//  reorder named actual arguments to match the order expected by the inner
//      function, i.e. the order of the formals (used to be order wrapper)
//      (C does not support named argument passing.)
//
//  coercion wrapper -- add explicit casts to perform type coercions known only
//      to Chapel.
//      (C does not support base-class coercions, etc.)
//
//  promotion wrapper -- replaces implicit array traversals with explicit
//      array traversals.
//      (C has no notion of scalar operator promotion.)
//
//////////////////////////////////////////////////////////////////////////////

#include "wrappers.h"

#include "astutil.h"
#include "build.h"
#include "caches.h"
#include "callInfo.h"
#include "driver.h"
#include "expr.h"
#include "ForLoop.h"
#include "passes.h"
#include "resolution.h"
#include "resolveIntents.h"
#include "stlUtil.h"
#include "stmt.h"
#include "stringutil.h"
#include "symbol.h"
#include "visibleFunctions.h"

static FnSymbol*  wrapDefaultedFormals(
                               FnSymbol*                fn,
                               CallInfo&                info,
                               std::vector<ArgSymbol*>& actualToFormal);

static void       reorderActuals(FnSymbol*                fn,
                                 CallInfo&                info,
                                 std::vector<ArgSymbol*>& actualIdxToFormal);

static void       coerceActuals(FnSymbol* fn,
                                CallInfo& info);

static bool       isPromotionRequired(FnSymbol* fn, CallInfo& info);

static FnSymbol*  promotionWrap(FnSymbol* fn,
                                CallInfo& info,
                                bool      buildFastFollowerChecks);

static FnSymbol*  buildEmptyWrapper(FnSymbol* fn, CallInfo& info);

static ArgSymbol* copyFormalForWrapper(ArgSymbol* formal);

/************************************* | **************************************
*                                                                             *
* The argument actualIdxToFormals[] contains the actuals for the call but     *
* shuffled, if necessary, to be in the same order as the formals.             *
*                                                                             *
************************************** | *************************************/

FnSymbol* wrapAndCleanUpActuals(FnSymbol*                fn,
                                CallInfo&                info,
                                std::vector<ArgSymbol*>  actualIdxToFormal,
                                bool                     fastFollowerChecks) {
  int       numActuals = static_cast<int>(actualIdxToFormal.size());
  FnSymbol* retval     = fn;

  if (numActuals < fn->numFormals()) {
    retval = wrapDefaultedFormals(retval, info, actualIdxToFormal);
  }

  // Map actuals to formals by position
  if (actualIdxToFormal.size() > 1) {
    reorderActuals(retval, info, actualIdxToFormal);
  }

  if (info.actuals.n > 0) {
    coerceActuals(retval, info);
  }

  if (isPromotionRequired(retval, info) == true) {
    retval = promotionWrap(retval, info, fastFollowerChecks);
  }

  return retval;
}

/************************************* | **************************************
*                                                                             *
* wrapDefaultedFormals()                                                      *
*                                                                             *
* "Wrap" a call with fewer actuals than the number of formals.                *
* This implies that the Chapel procedure has formals with "default" values.   *
*                                                                             *
* This is handled by finding/generating a procedure with the same number of   *
* formals as the call's actuals and then providing the default value for the  *
* unspecified formals.                                                        *
*                                                                             *
************************************** | *************************************/

static FnSymbol* buildWrapperForDefaultedFormals(FnSymbol*     fn,
                                                 CallInfo&     info,
                                                 Vec<Symbol*>* defaults,
                                                 SymbolMap*    paramMap);

static void      formalIsNotDefaulted(FnSymbol*  fn,
                                      ArgSymbol* formal,
                                      CallExpr*  call,
                                      FnSymbol*  wrapFn,
                                      SymbolMap& copyMap,
                                      SymbolMap* paramMap);

static void      updateWrapCall(FnSymbol*  fn,
                                ArgSymbol* formal,
                                CallExpr*  call,
                                FnSymbol*  wrapFn,
                                Symbol*    temp,
                                SymbolMap& copyMap,
                                SymbolMap* paramMap);

static void      formalIsDefaulted(FnSymbol*  fn,
                                   ArgSymbol* formal,
                                   CallExpr*  call,
                                   FnSymbol*  wrapFn,
                                   SymbolMap& copyMap);

static void      defaultedFormalApplyDefaultForType(ArgSymbol* formal,
                                                    FnSymbol*  wrapFn,
                                                    VarSymbol* temp);

static void      insertWrappedCall(FnSymbol* fn,
                                   FnSymbol* wrapper,
                                   CallExpr* call);

static FnSymbol* wrapDefaultedFormals(FnSymbol*                fn,
                                      CallInfo&                info,
                                      std::vector<ArgSymbol*>& actualFormals) {
  Vec<Symbol*> defaults;
  int          j      = 1;
  FnSymbol*    retval = NULL;

  for_formals(formal, fn) {
    bool used = false;

    for_vector(ArgSymbol, arg, actualFormals) {
      if (arg == formal) {
        used = true;
      }
    }

    if (used == false) {
      defaults.add(formal);
    }
  }

  retval = checkCache(defaultsCache, fn, &defaults);

  if (retval == NULL) {
    retval = buildWrapperForDefaultedFormals(fn, info, &defaults, &paramMap);

    resolveFormals(retval);

    addCache(defaultsCache, fn, retval, &defaults);
  }

  // update actualFormals[] for use in reorderActuals
  for_formals(formal, fn) {
    for (size_t i = 0; i < actualFormals.size(); i++) {
      if (actualFormals[i] == formal) {
        actualFormals[i] = retval->getFormal(j++);
      }
    }
  }

  return retval;
}

static FnSymbol* buildWrapperForDefaultedFormals(FnSymbol*     fn,
                                                 CallInfo&     info,
                                                 Vec<Symbol*>* defaults,
                                                 SymbolMap*    paramMap) {
  SET_LINENO(fn);

  FnSymbol* wrapper = buildEmptyWrapper(fn, info);

  // Prevent name-clash in generated code.
  // Also, provide a hint where this fcn came from.
  wrapper->cname = astr("_default_wrap_", fn->cname);

  // Mimic return type.
  if (!fn->isIterator()) {
    wrapper->retType = fn->retType;
  }

  SymbolMap copy_map;

  bool specializeDefaultConstructor =
    fn->hasFlag(FLAG_DEFAULT_CONSTRUCTOR) &&
    !fn->_this->type->symbol->hasFlag(FLAG_REF);

  if (specializeDefaultConstructor) {
    wrapper->removeFlag(FLAG_COMPILER_GENERATED);
    wrapper->_this = fn->_this->copy();

    copy_map.put(fn->_this, wrapper->_this);

    wrapper->insertAtTail(new DefExpr(wrapper->_this));

    if (defaults->v[defaults->n-1]->hasFlag(FLAG_IS_MEME)) {
      if (!isRecord(fn->_this->type) && !isUnion(fn->_this->type)) {
        wrapper->insertAtTail(new CallExpr(PRIM_MOVE,
                                           wrapper->_this,
                                           callChplHereAlloc(wrapper->_this->typeInfo())));

        wrapper->insertAtTail(new CallExpr(PRIM_SETCID, wrapper->_this));
      }
    }

    wrapper->insertAtTail(new CallExpr(PRIM_INIT_FIELDS, wrapper->_this));
  }

  CallExpr* call = new CallExpr(fn);

  call->square = info.call->square;    // Copy square brackets call flag.

  for_formals(formal, fn) {
    SET_LINENO(formal);

    if (defaults->in(formal) == NULL) {
      formalIsNotDefaulted(fn, formal, call, wrapper, copy_map, paramMap);

    } else if (paramMap->get(formal) != NULL) {
      // handle instantiated param formals
      call->insertAtTail(paramMap->get(formal));

    } else if (formal->hasFlag(FLAG_IS_MEME) == true) {
      formal->type = wrapper->_this->type;

      call->insertAtTail(wrapper->_this);

    } else {
      formalIsDefaulted(fn, formal, call, wrapper, copy_map);
    }
  }

  update_symbols(wrapper->body, &copy_map);

  insertWrappedCall(fn, wrapper, call);

  normalize(wrapper);

  return wrapper;
}

// The call provides an actual for this formal.  The wrap function should
// accept this actual and pass it to the underlying function.
static void formalIsNotDefaulted(FnSymbol*  fn,
                                 ArgSymbol* formal,
                                 CallExpr*  call,
                                 FnSymbol*  wrapFn,
                                 SymbolMap& copyMap,
                                 SymbolMap* paramMap) {
  ArgSymbol* wrapFnFormal = copyFormalForWrapper(formal);

  wrapFn->insertFormalAtTail(wrapFnFormal);

  // If the formal has a param value, then wrapFormal should have same value
  if (Symbol* value = paramMap->get(formal)) {
    paramMap->put(wrapFnFormal, value);
  }

  if (fn->_this == formal) {
    wrapFn->_this = wrapFnFormal;
  }

  if (formal->hasFlag(FLAG_IS_MEME) == true &&
      wrapFn->_this                 != NULL) {
    Symbol* _this = wrapFn->_this;

    _this->defPoint->insertAfter(new CallExpr(PRIM_MOVE, _this, wrapFnFormal));
  }

  if (formal->type->symbol->hasFlag(FLAG_REF) == true) {
    Symbol*   temp         = newTemp("wrap_ref_arg");
    CallExpr* addrOfFormal = new CallExpr(PRIM_ADDR_OF, wrapFnFormal);

    temp->addFlag(FLAG_MAYBE_PARAM);

    wrapFn->insertAtTail(new DefExpr(temp));
    wrapFn->insertAtTail(new CallExpr(PRIM_MOVE, temp, addrOfFormal));

    updateWrapCall(fn, formal, call, wrapFn, temp, copyMap, paramMap);

  // Formal has a type expression attached and is array/dom/dist
  } else if (fn->hasFlag(FLAG_DEFAULT_CONSTRUCTOR)      == true  &&
             fn->_this->type->symbol->hasFlag(FLAG_REF) == false &&
             wrapFnFormal->typeExpr                     != NULL  &&
             isRecordWrappedType(wrapFnFormal->type)    == true) {
    Symbol*        temp      = newTemp("wrap_type_arg");
    AggregateType* _thisType = toAggregateType(fn->_this->type);
    BlockStmt*      typeExpr = wrapFnFormal->typeExpr->copy();
    CallExpr*       initExpr = NULL;

    if (Symbol* field = _thisType->getField(formal->name, false)) {
      if (field->defPoint->parentSymbol == _thisType->symbol) {
        temp->addFlag(FLAG_INSERT_AUTO_DESTROY);
      }
    }

    wrapFn->insertAtTail(new DefExpr(temp));

    for_alist(expr, typeExpr->body) {
      wrapFn->insertAtTail(expr->remove());
    }

    initExpr = new CallExpr(PRIM_INIT, wrapFn->body->body.tail->remove());

    wrapFn->insertAtTail(new CallExpr(PRIM_MOVE, temp, initExpr));
    wrapFn->insertAtTail(new CallExpr("=",       temp, wrapFnFormal));

    updateWrapCall(fn, formal, call, wrapFn, temp,         copyMap, paramMap);

  } else {
    updateWrapCall(fn, formal, call, wrapFn, wrapFnFormal, copyMap, paramMap);
  }
}

static void updateWrapCall(FnSymbol*  fn,
                           ArgSymbol* formal,
                           CallExpr*  call,
                           FnSymbol*  wrapFn,
                           Symbol*    temp,
                           SymbolMap& copyMap,
                           SymbolMap* paramMap) {
  copyMap.put(formal, temp);

  call->insertAtTail(temp);

  if (fn->hasFlag(FLAG_DEFAULT_CONSTRUCTOR)      == true  &&
      fn->_this->type->symbol->hasFlag(FLAG_REF) == false &&
      strcmp(fn->name, "_construct__tuple")      != 0     &&
      formal->hasFlag(FLAG_TYPE_VARIABLE)        == false &&
      paramMap->get(formal)                      == NULL  &&
      formal->type                               != dtMethodToken) {
    Symbol*        _this     = wrapFn->_this;
    AggregateType* _thisType = toAggregateType(_this->type);

    if (Symbol* field = _thisType->getField(formal->name, false)) {
      Symbol* parent = field->defPoint->parentSymbol;

      if (parent == _thisType->symbol) {
        Symbol*   tmp      = newTemp("wrap_arg");
        Symbol*   name     = new_CStringSymbol(formal->name);
        CallExpr* autoCopy = new CallExpr("chpl__autoCopy", temp);

        wrapFn->insertAtTail(new DefExpr(tmp));

        wrapFn->insertAtTail(new CallExpr(PRIM_MOVE, tmp, autoCopy));

        wrapFn->insertAtTail(new CallExpr(PRIM_SET_MEMBER, _this, name, tmp));

        copyMap.put(formal, tmp);

        call->argList.tail->replace(new SymExpr(tmp));
      }
    }
  }
}

static void formalIsDefaulted(FnSymbol*  fn,
                              ArgSymbol* formal,
                              CallExpr*  call,
                              FnSymbol*  wrapFn,
                              SymbolMap& copyMap) {
  const char* temp_name                    = astr("default_arg", formal->name);
  VarSymbol*  temp                         = newTemp(temp_name);
  IntentTag   intent                       = formal->intent;
  bool        specializeDefaultConstructor = false;

  if (fn->hasFlag(FLAG_DEFAULT_CONSTRUCTOR)      == true &&
      fn->_this->type->symbol->hasFlag(FLAG_REF) == false) {
    specializeDefaultConstructor = true;
  }

  if (formal->type   != dtTypeDefaultToken &&
      formal->type   != dtMethodToken      &&
      formal->intent == INTENT_BLANK) {
    intent = blankIntentForType(formal->type);
  }

  if (intent != INTENT_INOUT && intent != INTENT_OUT) {
    temp->addFlag(FLAG_MAYBE_PARAM);
    temp->addFlag(FLAG_EXPR_TEMP);
  }

  if (formal->hasFlag(FLAG_TYPE_VARIABLE)) {
    temp->addFlag(FLAG_TYPE_VARIABLE);
  }

  copyMap.put(formal, temp);

  wrapFn->insertAtTail(new DefExpr(temp));

  if (intent == INTENT_OUT ||
      !formal->defaultExpr ||
      (formal->defaultExpr->body.length == 1 &&
       isSymExpr(formal->defaultExpr->body.tail) &&
       toSymExpr(formal->defaultExpr->body.tail)->symbol() == gTypeDefaultToken)) {
    defaultedFormalApplyDefaultForType(formal, wrapFn, temp);

  } else {
    // use argument default for the formal argument
    BlockStmt* defaultExpr = formal->defaultExpr->copy();

    for_alist(expr, defaultExpr->body) {
      wrapFn->insertAtTail(expr->remove());
    }

    // Normally, addLocalCopiesAndWritebacks will handle
    // adding the copies. However, because of some issues with
    // default constructors, the copy is added here for them.
    // (In particular, the called constructor function does not
    //  include the necessary copies, because it would interfere
    //  with the array-domain link in
    //    record { var D={1..2}; var A:[D] int }
    //  )
    if (specializeDefaultConstructor) {
      // Copy construct from the default value.
      // Sometimes, normalize has already added an initCopy in the
      // defaultExpr. But if it didn't, we need to add a copy.
      Expr* fromExpr      = wrapFn->body->body.tail->remove();
      bool  needsInitCopy = true;

      if (CallExpr* fromCall = toCallExpr(fromExpr)) {
        Expr* base = fromCall->baseExpr;

        if (UnresolvedSymExpr* urse = toUnresolvedSymExpr(base)) {
          if (0 == strcmp(urse->unresolved, "chpl__initCopy") ||
              0 == strcmp(urse->unresolved, "_createFieldDefault")) {
            needsInitCopy = false;
          }

        } else {
          INT_ASSERT(0); // if resolved, check for FLAG_INIT_COPY_FN
        }
      }

      if (needsInitCopy) {
        fromExpr = new CallExpr("chpl__initCopy", fromExpr);
      }

      wrapFn->insertAtTail(new CallExpr(PRIM_MOVE, temp, fromExpr));

    } else {
      // Otherwise, just pass it in
      if (intent & INTENT_FLAG_REF) {
        // For a ref intent argument, pass in address
        wrapFn->insertAtTail(new CallExpr(PRIM_MOVE, temp, new CallExpr(PRIM_ADDR_OF, wrapFn->body->body.tail->remove())));

      } else {
        wrapFn->insertAtTail(new CallExpr(PRIM_MOVE, temp, wrapFn->body->body.tail->remove()));
      }
    }

    if (formal->intent == INTENT_INOUT) {
      INT_ASSERT(!temp->hasFlag(FLAG_EXPR_TEMP));
      temp->removeFlag(FLAG_MAYBE_PARAM);
    }
  }

  call->insertAtTail(temp);


  // MPF - this seems really strange since it is assigning to
  // fields that will be set in the construct call at the end.
  // It is handling the current issue that an iterator to
  // initialize an array can refer to the fields.
  // See arrayDomInClassRecord2.chpl.
  // In the future, it would probably be better to initialize the
  // fields in order in favor of calling the default constructor.
  if (specializeDefaultConstructor          == true &&
      strcmp(fn->name, "_construct__tuple") != 0) {
    if (formal->hasFlag(FLAG_TYPE_VARIABLE) == false) {
      AggregateType* type = toAggregateType(wrapFn->_this->type);

      if (Symbol* field = type->getField(formal->name, false)) {
        if (field->defPoint->parentSymbol == type->symbol) {
          VarSymbol* name = new_CStringSymbol(formal->name);

          wrapFn->insertAtTail(new CallExpr(PRIM_SET_MEMBER,
                                             wrapFn->_this,
                                             name,
                                             temp));
        }
      }
    }
  }
}

static void defaultedFormalApplyDefaultForType(ArgSymbol* formal,
                                               FnSymbol*  wrapFn,
                                               VarSymbol* temp) {
  // use default value for type as default value for formal argument
  if (formal->typeExpr) {
    BlockStmt* typeExpr = formal->typeExpr->copy();

    for_alist(expr, typeExpr->body) {
      wrapFn->insertAtTail(expr->remove());
    }

    Expr* lastExpr = wrapFn->body->body.tail;

    if (formal->hasFlag(FLAG_TYPE_VARIABLE)) {
      wrapFn->insertAtTail(new CallExpr(PRIM_MOVE, temp, lastExpr->remove()));

    } else {
      //
      // 2016-07-18: benharsh: I was encountering an issue where we were
      // attempting to wrap a function where we had inserted return temps
      // for chpl__buildArrayRuntimeType. This wrapping function then
      // created an invalid AST like this:
      //
      // (move call_tmp (move _return_tmp_ (call chpl__buildArrayRuntimeType ...)))
      //
      // With this change we assume that if the last Expr is a PRIM_MOVE
      // that we can use the LHS of that move in the PRIM_INIT call that
      // needs to be inserted.
      //
      // The test that exposed this issue is:
      //   test/arrays/diten/distArrInRecord.chpl
      //
      // Compiled with -suseBulkTransferStride
      //
      CallExpr* lastCall = toCallExpr(lastExpr);

      if (lastCall != NULL && lastCall->isPrimitive(PRIM_MOVE)) {
        wrapFn->insertAtTail(new CallExpr(PRIM_MOVE, temp, new CallExpr(PRIM_INIT, lastCall->get(1)->copy())));

      } else {
        wrapFn->insertAtTail(new CallExpr(PRIM_MOVE, temp, new CallExpr(PRIM_INIT, lastExpr->remove())));
      }
    }

  } else {
    if (formal->hasFlag(FLAG_TYPE_VARIABLE)) {
      wrapFn->insertAtTail(new CallExpr(PRIM_MOVE, temp, new SymExpr(formal->type->symbol)));

    } else {
      wrapFn->insertAtTail(new CallExpr(PRIM_MOVE, temp, new CallExpr(PRIM_INIT, new SymExpr(formal->type->symbol))));
    }
  }
}

static void insertWrappedCall(FnSymbol* fn,
                              FnSymbol* wrapper,
                              CallExpr* call) {
  if (fn->getReturnSymbol() == gVoid || fn->retType == dtVoid) {
    wrapper->insertAtTail(call);

  } else {
    Symbol* tmp = newTemp("wrap_call_tmp");

    tmp->addFlag(FLAG_EXPR_TEMP);
    tmp->addFlag(FLAG_MAYBE_PARAM);
    tmp->addFlag(FLAG_MAYBE_TYPE);

    wrapper->insertAtTail(new DefExpr(tmp));
    wrapper->insertAtTail(new CallExpr(PRIM_MOVE,   tmp, call));
    wrapper->insertAtTail(new CallExpr(PRIM_RETURN, tmp));
  }

  fn->defPoint->insertAfter(new DefExpr(wrapper));
}

/************************************* | **************************************
*                                                                             *
* reorder the actuals to match the order of the formals                       *
*                                                                             *
************************************** | *************************************/

static void reorderActuals(FnSymbol*                fn,
                           CallInfo&                info,
                           std::vector<ArgSymbol*>& actualFormals) {
  int              numArgs       = actualFormals.size();
  std::vector<int> formalsToFormals(numArgs);
  bool             needToReorder = false;
  int              i             = 0;

  for_formals(formal, fn) {
    int j = 0;

    i++;

    for_vector(ArgSymbol, af, actualFormals) {
      j++;

      if (af == formal) {
        if (i != j) {
          needToReorder = true;
        }

        formalsToFormals[i - 1] = j - 1;
      }
    }
  }

  if (needToReorder == true) {
    std::vector<Expr*>       savedActuals(numArgs);
    std::vector<Symbol*>     ciActuals(numArgs);
    std::vector<const char*> ciActualNames(numArgs);
    int                      index = 0;

    // remove all actuals in an order
    for_actuals(actual, info.call) {
      savedActuals[index++] = actual->remove();
    }

    // reinsert them in the desired order
    for (int i = 0; i < numArgs; i++) {
      info.call->insertAtTail(savedActuals[formalsToFormals[i]]);
    }

    // reorder CallInfo data as well
    // ideally this would be encapsulated in within the CallInfo class
    INT_ASSERT(info.actuals.n == numArgs);

    for (int i = 0; i < numArgs; i++) {
      ciActuals[i]     = info.actuals.v[i];
      ciActualNames[i] = info.actualNames.v[i];
    }

    for (int i = 0; i < numArgs; i++) {
      info.actuals.v[i]     = ciActuals[formalsToFormals[i]];
      info.actualNames.v[i] = ciActualNames[formalsToFormals[i]];
    }
  }
}

/************************************* | **************************************
*                                                                             *
* add coercions on the actuals                                                *
*                                                                             *
************************************** | *************************************/

static bool      needToAddCoercion(Type*      actualType,
                                   Symbol*    actualSym,
                                   ArgSymbol* formal,
                                   FnSymbol*  fn);

static IntentTag getIntent(ArgSymbol* formal);

static void      addArgCoercion(FnSymbol*  fn,
                                CallExpr*  call,
                                ArgSymbol* formal,
                                Expr*&     actualExpr,
                                Symbol*&   actualSym,
                                bool&      checkAgain);

static void coerceActuals(FnSymbol* fn, CallInfo& info) {
  if (fn->retTag == RET_PARAM) {
    //
    // This call will be tossed in postFold(), so why bother with coercions?
    //
    // Most importantly, we don't want a readFE-like coercion in this case,
    // because the coercion will stick around even if the call is removed.
    //
    // Todo: postFold() will remove some other calls, too. However we don't
    // know which - until 'fn' is resolved, which here it may not be, yet.
    // So for now we act only if fn has the param retTag.
    //
    // The runner-up todo would be 'type' functions, which actually
    // may need to be invoked at run time if they return a runtime type.
    // Therefore "coercions" might also be needed, e.g., to readFE from
    // a sync var actual to determine the size of the array type's domain.
    // So we will keep the coercions uniformly for now, as if they are
    // a part of type functions' semantics.
    //
    return;
  }

  int   j          = -1;
  Expr* currActual = info.call->get(1);

  for_formals(formal, fn) {
    j++;
    Symbol* actualSym  = info.actuals.v[j];
    Type*   formalType = formal->type;
    bool    c2         = false;
    int     checksLeft = 6;

    // There does not seem to be a limit of how many coercions will be
    // needed for a given actual. For example, in myExpr.someFun(...),
    // each level of _syncvar(T) in myExpr's type adds two coercions,
    // PRIM_DEREF and CallExpr("value",...), to the coercions needed by T.
    //
    // Note: if we take away the special handling of a sync/single actual
    // when it is the receiver to 'fn' (the "value" case above), fewer
    // coercions will suffice for the same number of _syncvar layers.
    //
    // We could have the do-loop below terminate only upon !c2. For now,
    // I am putting a limit on the number of iterations just in case.
    // I am capping it at 6 arbitrarily. This allows for the 5 coercions
    // plus 1 last check in the case of a receiver actual of the type
    // _ref(_syncvar(_syncvar(int))), e.g. an array element "sync sync int".
    // -vass 8'2014
    //

    do {
      Type* actualType = actualSym->type;

      c2 = false;

      if (needToAddCoercion(actualType, actualSym, formal, fn)) {
        if (formalType               == dtStringC &&
            actualType               == dtString  &&
            actualSym->isImmediate() == true) {
          // We do this swap since we know the string is a valid literal
          // There also is no cast defined for string->c_string on purpose (you
          // need to use .c_str()) so the common case below does not work.
          VarSymbol*  var       = toVarSymbol(actualSym);
          const char* str       = var->immediate->v_string;
          SymExpr*    newActual = new SymExpr(new_CStringSymbol(str));

          currActual->replace(newActual);

          currActual = newActual;

        } else {
          addArgCoercion(fn, info.call, formal, currActual, actualSym, c2);
        }
      }
    } while (c2 && --checksLeft > 0);

    INT_ASSERT(c2 == false);

    currActual = currActual->next;
  }
}

// do we need to add some coercion from the actual to the formal?
static bool needToAddCoercion(Type*      actualType,
                              Symbol*    actualSym,
                              ArgSymbol* formal,
                              FnSymbol*  fn) {
  Type* formalType = formal->type;
  bool  retval     = false;

  if (actualType == formalType) {
    retval = false;

  // If we have an actual of ref(formalType) and
  // a REF or CONST REF argument intent, no coercion is necessary.
  } else if (actualType == formalType->getRefType() &&
             (getIntent(formal) & INTENT_FLAG_REF) != 0) {
    retval = false;

  } else if (canCoerce(actualType, actualSym, formalType, fn) == true) {
    retval =  true;

  } else if (isDispatchParent(actualType, formalType)         == true) {
    retval =  true;

  } else {
    retval = false;
  }

  return retval;
}

static IntentTag getIntent(ArgSymbol* formal) {
  IntentTag retval = formal->intent;

  if (retval == INTENT_BLANK || retval == INTENT_CONST) {
    if (formal->type->symbol->hasFlag(FLAG_ITERATOR_RECORD) == false) {
      retval = concreteIntentForArg(formal);
    }
  }

  return retval;
}

// Add a coercion; replace prevActual and actualSym - the actual to 'call' -
// with the result of the coercion.
static void addArgCoercion(FnSymbol*  fn,
                           CallExpr*  call,
                           ArgSymbol* formal,
                           Expr*&     actualExpr,
                           Symbol*&   actualSym,
                           bool&      checkAgain) {
  SET_LINENO(actualExpr);

  Expr*       prevActual = actualExpr;
  TypeSymbol* ats        = actualSym->type->symbol;
  TypeSymbol* fts        = formal->type->symbol;
  CallExpr*   castCall   = NULL;
  VarSymbol*  castTemp   = newTemp("coerce_tmp"); // ..., formal->type ?
  Expr*       newActual  = new SymExpr(castTemp);

  castTemp->addFlag(FLAG_COERCE_TEMP);

  // gotta preserve this-ness, so can write to this's fields in constructors
  if (actualSym->hasFlag(FLAG_ARG_THIS) &&
      isDispatchParent(actualSym->type, formal->type)) {
    castTemp->addFlag(FLAG_ARG_THIS);
  }

  if (NamedExpr* namedActual = toNamedExpr(prevActual)) {
    // preserve the named portion
    Expr* newCurrActual = namedActual->actual;

    newCurrActual->replace(newActual);

    newActual  = prevActual;
    prevActual = newCurrActual;

  } else {
    prevActual->replace(newActual);
  }

  // Now 'prevActual' has been removed+replaced and is ready to be passed
  // as an actual to a cast or some such.
  // We can update addArgCoercion's caller right away.
  actualExpr = newActual;
  actualSym  = castTemp;

  // Here we will often strip the type of its sync-ness.
  // After that we may need another coercion(s), e.g.
  //   _syncvar(int) --readFE()-> _ref(int) --(dereference)-> int --> real
  // or
  //   _syncvar(_syncvar(int))  -->...  _syncvar(int)  -->  [as above]
  //
  // We warn addArgCoercion's caller about that via checkAgain:
  if (isSyncType(ats->type) == true) {
    checkAgain = true;
    castCall   = new CallExpr("readFE", gMethodToken, prevActual);

  } else if (isSingleType(ats->type) == true) {
    checkAgain = true;

    castCall   = new CallExpr("readFF", gMethodToken, prevActual);

  } else if (ats->hasFlag(FLAG_REF) &&
             !(ats->getValType()->symbol->hasFlag(FLAG_TUPLE) &&
               formal->getValType()->symbol->hasFlag(FLAG_TUPLE)) ) {
    //
    // dereference a reference actual
    //
    // after dereferencing we may need another coercion, e.g.
    //   _ref(int)  --coerce->  int  --coerce->  real
    // or
    //   _ref(_syncvar(int)) --> _syncvar(int) --> _ref(int) --> int --> real
    //
    checkAgain = true;

    // MPF - this call here is suspect because dereferencing should
    // call a record's copy-constructor (e.g. autoCopy).
    castCall   = new CallExpr(PRIM_DEREF, prevActual);

    if (SymExpr* prevSE = toSymExpr(prevActual)) {
      if (prevSE->symbol()->hasFlag(FLAG_REF_TO_CONST)) {
        castTemp->addFlag(FLAG_CONST);

        if (prevSE->symbol()->hasFlag(FLAG_REF_FOR_CONST_FIELD_OF_THIS)) {
          castTemp->addFlag(FLAG_REF_FOR_CONST_FIELD_OF_THIS);
        }
      }
    }

  } else {
    // There was code to handle the case when the flag *is* present.
    // I deleted that code. The assert ensures it wouldn't apply anyway.
    INT_ASSERT(!actualSym->hasFlag(FLAG_INSTANTIATED_PARAM));

    castCall = NULL;
  }

  if (castCall == NULL) {
    // the common case
    castCall = createCast(prevActual, fts);

    if (isString(fts)) {
      castTemp->addFlag(FLAG_INSERT_AUTO_DESTROY);
    }
  }

  // move the result to the temp
  CallExpr* castMove = new CallExpr(PRIM_MOVE, castTemp, castCall);

  call->getStmtExpr()->insertBefore(new DefExpr(castTemp));
  call->getStmtExpr()->insertBefore(castMove);

  resolveCallAndCallee(castCall, true);

  if (FnSymbol* castTarget = castCall->resolvedFunction()) {
    // Perhaps equivalently, we could check "if (tryToken)",
    // except tryToken is not visible in this file.
    if (!castTarget->hasFlag(FLAG_RESOLVED)) {
      // This happens e.g. when castTarget itself has an error.
      // Todo: in this case, we should report the error at the point
      // where it arises, supposedly within resolveFns(castTarget).
      // Why is it not reported there?
      USR_FATAL_CONT(call,
                     "Error resolving a cast from %s to %s",
                     ats->name,
                     fts->name);

      USR_PRINT(castTarget, "  the troublesome function is here");
      USR_STOP();
    }
  }

  resolveCall(castMove);
}

/************************************* | **************************************
*                                                                             *
*                                                                             *
*                                                                             *
************************************** | *************************************/

static FnSymbol*
buildPromotionWrapper(FnSymbol*  fn,
                      CallInfo&  info,
                      bool       buildFastFollowerChecks,
                      SymbolMap* promotionSubs);

static void
fixUnresolvedSymExprsForPromotionWrapper(FnSymbol* wrapper,
                                         FnSymbol* fn);

static void
buildPromotionFastFollowerCheck(bool                  isStatic,
                                bool                  addLead,
                                CallInfo&             info,
                                FnSymbol*             wrapper,
                                std::set<ArgSymbol*>& requiresPromotion);

static bool isPromotionRequired(FnSymbol* fn, CallInfo& info) {
  bool retval = false;

  if (fn->name != astrSequals && fn->hasFlag(FLAG_TYPE_CONSTRUCTOR) == false) {
    int j = 0;

    for_formals(formal, fn) {
      Symbol* actual     = info.actuals.v[j++];
      Type*   actualType = actual->type;
      bool    promotes   = false;

      if (isRecordWrappedType(actualType) == true) {
        makeRefType(actualType);

        actualType = actualType->refType;

        INT_ASSERT(actualType);
      }

      if (canDispatch(actualType, actual, formal->type, fn, &promotes)) {
        if (promotes == true) {
          retval = true;
          break;
        }
      }
    }
  }

  return retval;
}

static FnSymbol* promotionWrap(FnSymbol* fn,
                               CallInfo& info,
                               bool      buildFastFollowerChecks) {

  Vec<Symbol*>* actuals = &info.actuals;

  if (fn->name == astrSequals) {
    return fn;
  }

  // Don't try to promotion wrap _ref type constructor
  if (fn->hasFlag(FLAG_TYPE_CONSTRUCTOR)) {
    return fn;
  }

  bool      promotion_wrapper_required = false;
  int       j                          = -1;
  SymbolMap promoted_subs;

  for_formals(formal, fn) {
    j++;

    Type*   actualType = actuals->v[j]->type;
    Symbol* actualSym  = actuals->v[j];
    bool    promotes   = false;

    if (isRecordWrappedType(actualType)) {
      makeRefType(actualType);

      actualType = actualType->refType;

      INT_ASSERT(actualType);
    }

    if (canDispatch(actualType, actualSym, formal->type, fn, &promotes)) {
      if (promotes) {
        promotion_wrapper_required = true;
        promoted_subs.put(formal, actualType->symbol);
      }
    }
  }

  if (promotion_wrapper_required) {
    if (fReportPromotion) {
      USR_WARN(info.call, "promotion on %s", info.toString());
    }

    FnSymbol* wrapper = checkCache(promotionsCache, fn, &promoted_subs);

    if (wrapper == NULL) {
      wrapper = buildPromotionWrapper(fn,
                                      info,
                                      buildFastFollowerChecks,
                                      &promoted_subs);

      addCache(promotionsCache, fn, wrapper, &promoted_subs);
    }

    resolveFormals(wrapper);

    return wrapper;

  } else {
    return fn;
  }
}

static FnSymbol* buildPromotionWrapper(FnSymbol*  fn,
                                       CallInfo&  info,
                                       bool       buildFastFollowerChecks,
                                       SymbolMap* promotion_subs) {
  SET_LINENO(info.call);

  FnSymbol* wrapper = buildEmptyWrapper(fn, info);

  wrapper->addFlag(FLAG_PROMOTION_WRAPPER);

  // Special case: When promoting a default constructor, the promotion wrapper
  // itself is no longer a default constructor.
  wrapper->removeFlag(FLAG_DEFAULT_CONSTRUCTOR);

  wrapper->cname = astr("_promotion_wrap_", fn->cname);

  std::set<ArgSymbol*> requiresPromotion;
  CallExpr*            indicesCall  = new CallExpr("_build_tuple");
  CallExpr*            iteratorCall = new CallExpr("_build_tuple");
  CallExpr*            actualCall   = new CallExpr(fn);
  bool                 zippered     = true;
  int                  i            = 1;

  for_formals(formal, fn) {
    SET_LINENO(formal);

    ArgSymbol* new_formal = copyFormalForWrapper(formal);

    if (Symbol* p = paramMap.get(formal)) {
      paramMap.put(new_formal, p);
    }

    if (fn->_this == formal) {
      wrapper->_this = new_formal;
    }

    if (Symbol* sub = promotion_subs->get(formal)) {
      TypeSymbol* ts = toTypeSymbol(sub);

      requiresPromotion.insert(new_formal);

      if (!ts) {
        INT_FATAL(fn, "error building promotion wrapper");
      }

      new_formal->type = ts->type;

      wrapper->insertFormalAtTail(new_formal);

      iteratorCall->insertAtTail(new_formal);

      // Rely on the 'destructureIndices' function in build.cpp to create a
      // VarSymbol and DefExpr for these indices. This solves a problem where
      // these 'p_i_' variables were declared outside of the loop body's scope.
      const char* name = astr("p_i_", istr(i));

      indicesCall->insertAtTail(new UnresolvedSymExpr(name));

      actualCall->insertAtTail(new UnresolvedSymExpr(name));

    } else {
      wrapper->insertFormalAtTail(new_formal);
      actualCall->insertAtTail(new_formal);
    }

    i++;
  }

  // Convert 1-tuples to their contents for the second half of this function
  Expr* indices = indicesCall;

  if (indicesCall->numActuals() == 1) {
    indices = indicesCall->get(1)->remove();
  }

  Expr* iterator = iteratorCall;

  if (iteratorCall->numActuals() == 1) {
    iterator = iteratorCall->get(1)->remove();
    zippered = false;
  }

  if ((!fn->hasFlag(FLAG_EXTERN) && fn->getReturnSymbol() == gVoid) ||
      (fn->hasFlag(FLAG_EXTERN) && fn->retType == dtVoid)) {
      wrapper->insertAtTail(new BlockStmt(buildForallLoopStmt(indices, iterator, /*byref_vars=*/ NULL, new BlockStmt(actualCall), zippered)));

  } else {
    wrapper->addFlag(FLAG_ITERATOR_FN);
    wrapper->removeFlag(FLAG_INLINE);


    // Build up the leader iterator
    SymbolMap leaderMap;
    FnSymbol* lifn       = wrapper->copy(&leaderMap);

    INT_ASSERT(! lifn->hasFlag(FLAG_RESOLVED));

    iteratorLeaderMap.put(wrapper,lifn);

    lifn->body = new BlockStmt(); // indices are not used in leader

    form_Map(SymbolMapElem, e, leaderMap) {
      if (Symbol* s = paramMap.get(e->key)) {
        paramMap.put(e->value, s);
      }
    }

    ArgSymbol* lifnTag = new ArgSymbol(INTENT_PARAM, "tag", gLeaderTag->type);

    // Leader iterators are always inlined.
    lifn->addFlag(FLAG_INLINE_ITERATOR);

    lifn->insertFormalAtTail(lifnTag);

    lifn->where = new BlockStmt(new CallExpr("==", lifnTag, gLeaderTag));

    VarSymbol* leaderIndex    = newTemp("p_leaderIndex");
    VarSymbol* leaderIterator = newTemp("p_leaderIterator");

    leaderIterator->addFlag(FLAG_EXPR_TEMP);

    lifn->insertAtTail(new DefExpr(leaderIterator));

    if (zippered == false) {
      lifn->insertAtTail(new CallExpr(PRIM_MOVE, leaderIterator, new CallExpr("_toLeader", iterator->copy(&leaderMap))));
    } else {
      lifn->insertAtTail(new CallExpr(PRIM_MOVE, leaderIterator, new CallExpr("_toLeaderZip", iterator->copy(&leaderMap))));
    }

    BlockStmt* body = new BlockStmt(new CallExpr(PRIM_YIELD, leaderIndex));
    BlockStmt* loop = ForLoop::buildForLoop(new SymExpr(leaderIndex), new SymExpr(leaderIterator), body, false, zippered);

    lifn->insertAtTail(loop);

    theProgram->block->insertAtTail(new DefExpr(lifn));

    toBlockStmt(body->parentExpr)->insertAtHead(new DefExpr(leaderIndex));

    normalize(lifn);

    lifn->addFlag(FLAG_GENERIC);
    lifn->instantiationPoint = getVisibilityBlock(info.call);

    // Build up the follower iterator
    SymbolMap followerMap;
    FnSymbol* fifn = wrapper->copy(&followerMap);

    INT_ASSERT(! fifn->hasFlag(FLAG_RESOLVED));

    iteratorFollowerMap.put(wrapper,fifn);

    form_Map(SymbolMapElem, e, followerMap) {
      if (Symbol* s = paramMap.get(e->key)) {
        paramMap.put(e->value, s);
      }
    }

    ArgSymbol* fifnTag      = new ArgSymbol(INTENT_PARAM, "tag", gFollowerTag->type);
    fifn->insertFormalAtTail(fifnTag);

    ArgSymbol* fifnFollower = new ArgSymbol(INTENT_BLANK, iterFollowthisArgname, dtAny);

    fifn->insertFormalAtTail(fifnFollower);

    ArgSymbol* fastFollower = new ArgSymbol(INTENT_PARAM, "fast", dtBool, NULL, new SymExpr(gFalse));

    fifn->insertFormalAtTail(fastFollower);

    fifn->where = new BlockStmt(new CallExpr("==", fifnTag, gFollowerTag));

    VarSymbol* followerIterator = newTemp("p_followerIterator");

    followerIterator->addFlag(FLAG_EXPR_TEMP);

    fifn->insertAtTail(new DefExpr(followerIterator));

    if (zippered == false) {
      fifn->insertAtTail(new CondStmt(new SymExpr(fastFollower),
                         new CallExpr(PRIM_MOVE, followerIterator, new CallExpr("_toFastFollower", iterator->copy(&followerMap), fifnFollower)),
                         new CallExpr(PRIM_MOVE, followerIterator, new CallExpr("_toFollower", iterator->copy(&followerMap), fifnFollower))));
    } else {
      fifn->insertAtTail(new CondStmt(new SymExpr(fastFollower),
                         new CallExpr(PRIM_MOVE, followerIterator, new CallExpr("_toFastFollowerZip", iterator->copy(&followerMap), fifnFollower)),
                         new CallExpr(PRIM_MOVE, followerIterator, new CallExpr("_toFollowerZip", iterator->copy(&followerMap), fifnFollower))));
    }

    BlockStmt* followerBlock = new BlockStmt();
    Symbol*    yieldTmp      = newTemp("p_yield");

    yieldTmp->addFlag(FLAG_EXPR_TEMP);

    followerBlock->insertAtTail(new DefExpr(yieldTmp));
    followerBlock->insertAtTail(new CallExpr(PRIM_MOVE, yieldTmp, actualCall->copy(&followerMap)));
    followerBlock->insertAtTail(new CallExpr(PRIM_YIELD, yieldTmp));

    fifn->insertAtTail(ForLoop::buildForLoop(indices->copy(&followerMap), new SymExpr(followerIterator), followerBlock, false, zippered));

    theProgram->block->insertAtTail(new DefExpr(fifn));

    normalize(fifn);

    fifn->addFlag(FLAG_GENERIC);

    fifn->instantiationPoint = getVisibilityBlock(info.call);

    fixUnresolvedSymExprsForPromotionWrapper(fifn, fn);

    if (!fNoFastFollowers && buildFastFollowerChecks) {
      // Build up the static (param) fast follower check functions
      buildPromotionFastFollowerCheck(/*isStatic=*/true,  /*addLead=*/false, info, wrapper, requiresPromotion);
      buildPromotionFastFollowerCheck(/*isStatic=*/true,  /*addLead=*/true,  info, wrapper, requiresPromotion);

      // Build up the dynamic fast follower check functions
      buildPromotionFastFollowerCheck(/*isStatic=*/false, /*addLead=*/false, info, wrapper, requiresPromotion);
      buildPromotionFastFollowerCheck(/*isStatic=*/false, /*addLead=*/true,  info, wrapper, requiresPromotion);
    }


    // Finish building the serial iterator. We stopped mid-way so the common
    // code could be copied for the leader/follower
    BlockStmt* yieldBlock = new BlockStmt();

    yieldTmp = newTemp("p_yield");

    yieldTmp->addFlag(FLAG_EXPR_TEMP);

    yieldBlock->insertAtTail(new DefExpr(yieldTmp));
    yieldBlock->insertAtTail(new CallExpr(PRIM_MOVE, yieldTmp, actualCall));
    yieldBlock->insertAtTail(new CallExpr(PRIM_YIELD, yieldTmp));

    wrapper->insertAtTail(new BlockStmt(ForLoop::buildForLoop(indices, iterator, yieldBlock, false, zippered)));
  }

  fn->defPoint->insertBefore(new DefExpr(wrapper));

  normalize(wrapper);

  fixUnresolvedSymExprsForPromotionWrapper(wrapper, fn);

  return wrapper;
}

static void fixUnresolvedSymExprsForPromotionWrapper(FnSymbol* wrapper,
                                                     FnSymbol* fn) {
  // Fix the UnresolvedSymExprs we inserted to the actualCall. For each
  // call to `fn`, pick out any UnresolvedSymExprs and look in the loop
  // body for a corresponding DefExpr.

  std::vector<CallExpr*> calls;

  collectCallExprs(wrapper, calls);

  for_vector(CallExpr, call, calls) {
    if (call->resolvedFunction() == fn) {
      for_actuals(actual, call) {
        if (UnresolvedSymExpr* unsym = toUnresolvedSymExpr(actual)) {
          std::vector<DefExpr*> defs;
          BlockStmt*            callBlock = NULL;
          BlockStmt*            loop      = NULL;
          bool                  found = false;

          callBlock = toBlockStmt(call->getStmtExpr()->parentExpr);
          loop      = toBlockStmt(callBlock->parentExpr);

          INT_ASSERT(loop && loop->isLoopStmt());

          collectDefExprs(loop, defs);

          for_vector(DefExpr, def, defs) {
            if (strcmp(def->sym->name, unsym->unresolved) == 0) {
              unsym->replace(new SymExpr(def->sym));

              found = true;
              break;
            }
          }

          INT_ASSERT(found);
        }
      }
    }
  }
}

//
// In order for fast followers to trigger, the invoking loop requires a static
// and dynamic check. They determine at compile time if the iterands implement
// a fast follower, and at runtime if all the iterands can fast follow the
// leader. Here we build up the checks for an iterator record. We basically
// convert the iterator record into a tuple and call the "zip" check. Note
// that we only stuff the components of the iterator record that actually
// require promotion into the tuple.
//
// i.e. we build up something like:
//
//    _iteratorRecord has a field for each formal in the promoted function.
//    `A + 2.0 * C` results in a record with fields for each array and the real
//
//    proc chpl__dynamicFastFollowCheck(x: _iteratorRecord, lead) {
//      // tuple that only has elements for fields that require promotion
//      var promotion_tup: recordToPromotionTuple(x);
//      var cur_tup_idx = 1;
//      for param fieldNum in 1..numFields(x.type) {
//        var field = getField(x.type, fieldNum);
//        if requiresPromotion(field.type, x) {
//          promotion_tup(cur_tup_idx) = field;
//          cur_tup_idx += 1;
//        }
//      }
//      return chpl__dynamicFastFollowCheckZip(promotion_tup, lead);
//    }
//
// However, since the iterator record isn't fully built (none of the fields
// exist yet), we use a primitive as a placeholder. When the record is filled
// in during iterator lowering, we replace the primitive with the actual field.
//
static void
buildPromotionFastFollowerCheck(bool                  isStatic,
                                bool                  addLead,
                                CallInfo&             info,
                                FnSymbol*             wrapper,
                                std::set<ArgSymbol*>& requiresPromotion) {
  const char* fnName = isStatic ? "chpl__staticFastFollowCheck" : "chpl__dynamicFastFollowCheck";
  const char* forwardFnName = astr(fnName, "Zip") ;

  FnSymbol* fastFollowCheckFn = new FnSymbol(fnName);

  if (isStatic) {
    fastFollowCheckFn->retTag = RET_PARAM;
  } else {
    fastFollowCheckFn->retTag = RET_VALUE;
  }

  ArgSymbol* x = new ArgSymbol(INTENT_BLANK, "x", dtIteratorRecord);

  fastFollowCheckFn->insertFormalAtTail(x);

  ArgSymbol* lead = new ArgSymbol(INTENT_BLANK, "lead", dtAny);

  if (addLead) {
    fastFollowCheckFn->insertFormalAtTail(lead);
  }


  CallExpr* buildTuple = new CallExpr("_build_tuple_always_allow_ref");

  for_formals(formal, wrapper) {
    if (requiresPromotion.count(formal) > 0) {
      Symbol* field = new VarSymbol(formal->name, formal->type);

      fastFollowCheckFn->insertAtTail(new DefExpr(field));

      fastFollowCheckFn->insertAtTail(new CallExpr(PRIM_MOVE, field, new CallExpr(PRIM_ITERATOR_RECORD_FIELD_VALUE_BY_FORMAL, x, formal)));

      buildTuple->insertAtTail(new SymExpr(field));
    }
  }

  fastFollowCheckFn->where = new BlockStmt(new CallExpr("==", new CallExpr(PRIM_TYPEOF, x), new CallExpr(PRIM_TYPEOF, info.call->copy())));

  Symbol* p_tup = newTemp("p_tup");

  fastFollowCheckFn->insertAtTail(new DefExpr(p_tup));
  fastFollowCheckFn->insertAtTail(new CallExpr(PRIM_MOVE, p_tup, buildTuple));

  Symbol* returnTmp = newTemp("p_ret");

  returnTmp->addFlag(FLAG_EXPR_TEMP);
  returnTmp->addFlag(FLAG_MAYBE_PARAM);

  fastFollowCheckFn->insertAtTail(new DefExpr(returnTmp));

  if (addLead) {
    fastFollowCheckFn->insertAtTail(new CallExpr(PRIM_MOVE, returnTmp, new CallExpr(forwardFnName, p_tup, lead)));
  } else {
    fastFollowCheckFn->insertAtTail(new CallExpr(PRIM_MOVE, returnTmp, new CallExpr(forwardFnName, p_tup)));
  }

  fastFollowCheckFn->insertAtTail(new CallExpr(PRIM_RETURN, returnTmp));

  theProgram->block->insertAtTail(new DefExpr(fastFollowCheckFn));

  normalize(fastFollowCheckFn);

  fastFollowCheckFn->addFlag(FLAG_GENERIC);

  fastFollowCheckFn->instantiationPoint = getVisibilityBlock(info.call);
}

/************************************* | **************************************
*                                                                             *
*                                                                             *
*                                                                             *
************************************** | *************************************/

static FnSymbol* buildEmptyWrapper(FnSymbol* fn, CallInfo& info) {
  FnSymbol* wrapper = new FnSymbol(fn->name);

  wrapper->addFlag(FLAG_WRAPPER);

  wrapper->addFlag(FLAG_INVISIBLE_FN);

  wrapper->addFlag(FLAG_INLINE);

  if (fn->hasFlag(FLAG_INIT_COPY_FN)) {
    wrapper->addFlag(FLAG_INIT_COPY_FN);
  }

  if (fn->hasFlag(FLAG_AUTO_COPY_FN)) {
    wrapper->addFlag(FLAG_AUTO_COPY_FN);
  }

  if (fn->hasFlag(FLAG_AUTO_DESTROY_FN)) {
    wrapper->addFlag(FLAG_AUTO_DESTROY_FN);
  }

  if (fn->hasFlag(FLAG_DONOR_FN)) {
    wrapper->addFlag(FLAG_DONOR_FN);
  }

  if (fn->hasFlag(FLAG_NO_PARENS)) {
    wrapper->addFlag(FLAG_NO_PARENS);
  }

  if (fn->hasFlag(FLAG_CONSTRUCTOR)) {
    wrapper->addFlag(FLAG_CONSTRUCTOR);
  }

  if (fn->hasFlag(FLAG_FIELD_ACCESSOR)) {
    wrapper->addFlag(FLAG_FIELD_ACCESSOR);
  }

  if (fn->hasFlag(FLAG_REF_TO_CONST)) {
    wrapper->addFlag(FLAG_REF_TO_CONST);
  }

  if (!fn->isIterator()) { // getValue is var, not iterator
    wrapper->retTag = fn->retTag;
  }

  if (fn->hasFlag(FLAG_METHOD)) {
    wrapper->addFlag(FLAG_METHOD);
  }

  if (fn->hasFlag(FLAG_METHOD_PRIMARY)) {
    wrapper->addFlag(FLAG_METHOD_PRIMARY);
  }

  if (fn->hasFlag(FLAG_ASSIGNOP)) {
    wrapper->addFlag(FLAG_ASSIGNOP);
  }

  wrapper->instantiationPoint = getVisibilityBlock(info.call);

  if (fn->hasFlag(FLAG_DEFAULT_CONSTRUCTOR)) {
    wrapper->addFlag(FLAG_DEFAULT_CONSTRUCTOR);
  }

  if (fn->hasFlag(FLAG_LAST_RESORT)) {
    wrapper->addFlag(FLAG_LAST_RESORT);
  }

  if (fn->hasFlag(FLAG_COMPILER_GENERATED)) {
    wrapper->addFlag(FLAG_WAS_COMPILER_GENERATED);
  }

  wrapper->addFlag(FLAG_COMPILER_GENERATED);

  if (fn->throwsError())
    wrapper->throwsErrorInit();

  return wrapper;
}

/************************************* | **************************************
*                                                                             *
* copy a formal and make the copy have blank intent. If the formal to copy    *
* has out intent or inout intent, flag the copy to make sure it is a          *
* reference.                                                                  *
*                                                                             *
* If the formal is ref intent, leave it as ref on the wrapper formal.         *
*                                                                             *
************************************** | *************************************/

static ArgSymbol* copyFormalForWrapper(ArgSymbol* formal) {
  ArgSymbol* wrapperFormal = formal->copy();

  if (formal->intent == INTENT_OUT ||
      formal->intent == INTENT_INOUT ||
      formal->hasFlag(FLAG_WRAP_WRITTEN_FORMAL)) {
    wrapperFormal->addFlag(FLAG_WRAP_WRITTEN_FORMAL);
  }

  if (formal->intent != INTENT_REF && formal->intent != INTENT_CONST_REF) {
    wrapperFormal->intent = INTENT_BLANK;
  }

  return wrapperFormal;
}
