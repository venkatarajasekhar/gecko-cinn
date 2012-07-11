/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MethodJIT.h"
#include "jsnum.h"
#include "jsbool.h"
#include "jsiter.h"
#include "Compiler.h"
#include "StubCalls.h"
#include "MonoIC.h"
#include "PolyIC.h"
#include "ICChecker.h"
#include "Retcon.h"
#include "assembler/jit/ExecutableAllocator.h"
#include "assembler/assembler/LinkBuffer.h"
#include "FrameState-inl.h"
#include "jsobjinlines.h"
#include "jsscriptinlines.h"
#include "InlineFrameAssembler.h"
#include "jscompartment.h"
#include "jsopcodeinlines.h"

#include "builtin/RegExp.h"
#include "vm/RegExpStatics.h"
#include "vm/RegExpObject.h"

#include "jsautooplen.h"
#include "jstypedarrayinlines.h"
#include "vm/RegExpObject-inl.h"

#include "ion/Ion.h"

using namespace js;
using namespace js::mjit;
#if defined(JS_POLYIC) || defined(JS_MONOIC)
using namespace js::mjit::ic;
#endif
using namespace js::analyze;

#define RETURN_IF_OOM(retval)                                   \
    JS_BEGIN_MACRO                                              \
        if (oomInVector || masm.oom() || stubcc.masm.oom())     \
            return retval;                                      \
    JS_END_MACRO

/*
 * Number of times a script must be called or had a backedge before we try to
 * inline its calls. This number should match IonMonkey's usesBeforeCompile.
 */
static const size_t USES_BEFORE_INLINING = 10240;

mjit::Compiler::Compiler(JSContext *cx, JSScript *outerScript,
                         unsigned chunkIndex, bool isConstructing)
  : BaseCompiler(cx),
    outerScript(cx, outerScript),
    chunkIndex(chunkIndex),
    isConstructing(isConstructing),
    outerChunk(outerJIT()->chunkDescriptor(chunkIndex)),
    ssa(cx, outerScript),
    globalObj(cx, outerScript->hasGlobal() ? outerScript->global() : NULL),
    globalSlots(globalObj ? globalObj->getRawSlots() : NULL),
    frame(cx, *thisFromCtor(), masm, stubcc),
    a(NULL), outer(NULL), script(NULL), PC(NULL), loop(NULL),
    inlineFrames(CompilerAllocPolicy(cx, *thisFromCtor())),
    branchPatches(CompilerAllocPolicy(cx, *thisFromCtor())),
#if defined JS_MONOIC
    getGlobalNames(CompilerAllocPolicy(cx, *thisFromCtor())),
    setGlobalNames(CompilerAllocPolicy(cx, *thisFromCtor())),
    callICs(CompilerAllocPolicy(cx, *thisFromCtor())),
    equalityICs(CompilerAllocPolicy(cx, *thisFromCtor())),
#endif
#if defined JS_POLYIC
    pics(CompilerAllocPolicy(cx, *thisFromCtor())),
    getElemICs(CompilerAllocPolicy(cx, *thisFromCtor())),
    setElemICs(CompilerAllocPolicy(cx, *thisFromCtor())),
#endif
    callPatches(CompilerAllocPolicy(cx, *thisFromCtor())),
    callSites(CompilerAllocPolicy(cx, *thisFromCtor())),
    doubleList(CompilerAllocPolicy(cx, *thisFromCtor())),
    rootedTemplates(CompilerAllocPolicy(cx, *thisFromCtor())),
    rootedRegExps(CompilerAllocPolicy(cx, *thisFromCtor())),
    fixedIntToDoubleEntries(CompilerAllocPolicy(cx, *thisFromCtor())),
    fixedDoubleToAnyEntries(CompilerAllocPolicy(cx, *thisFromCtor())),
    jumpTables(CompilerAllocPolicy(cx, *thisFromCtor())),
    jumpTableEdges(CompilerAllocPolicy(cx, *thisFromCtor())),
    loopEntries(CompilerAllocPolicy(cx, *thisFromCtor())),
    chunkEdges(CompilerAllocPolicy(cx, *thisFromCtor())),
    stubcc(cx, *thisFromCtor(), frame),
    debugMode_(cx->compartment->debugMode()),
    inlining_(false),
    hasGlobalReallocation(false),
    oomInVector(false),
    overflowICSpace(false),
    gcNumber(cx->runtime->gcNumber),
    pcLengths(NULL)
{
    /* Once a script starts getting really hot we will inline calls in it. */
    if (!debugMode() && cx->typeInferenceEnabled() && globalObj &&
        (outerScript->getUseCount() >= USES_BEFORE_INLINING ||
         cx->hasRunOption(JSOPTION_METHODJIT_ALWAYS))) {
        inlining_ = true;
    }
}

CompileStatus
mjit::Compiler::compile()
{
    JS_ASSERT(!outerChunkRef().chunk);

    CompileStatus status = performCompilation();
    if (status != Compile_Okay && status != Compile_Retry) {
        if (!outerScript->ensureHasJITInfo(cx))
            return Compile_Error;
        JSScript::JITScriptHandle *jith = outerScript->jitHandle(isConstructing, cx->compartment->needsBarrier());
        JSScript::ReleaseCode(cx->runtime->defaultFreeOp(), jith);
        jith->setUnjittable();

        if (outerScript->function()) {
            outerScript->uninlineable = true;
            types::MarkTypeObjectFlags(cx, outerScript->function(),
                                       types::OBJECT_FLAG_UNINLINEABLE);
        }
    }

    return status;
}

CompileStatus
mjit::Compiler::checkAnalysis(HandleScript script)
{
    if (script->hasClearedGlobal()) {
        JaegerSpew(JSpew_Abort, "script has a cleared global\n");
        return Compile_Abort;
    }

    if (!script->ensureRanAnalysis(cx, NULL))
        return Compile_Error;

    if (!script->analysis()->jaegerCompileable()) {
        JaegerSpew(JSpew_Abort, "script has uncompileable opcodes\n");
        return Compile_Abort;
    }

    if (cx->typeInferenceEnabled() && !script->ensureRanInference(cx))
        return Compile_Error;

    ScriptAnalysis *analysis = script->analysis();
    analysis->assertMatchingDebugMode();
    if (analysis->failed()) {
        JaegerSpew(JSpew_Abort, "couldn't analyze bytecode; probably switchX or OOM\n");
        return Compile_Abort;
    }

    return Compile_Okay;
}

CompileStatus
mjit::Compiler::addInlineFrame(HandleScript script, uint32_t depth,
                               uint32_t parent, jsbytecode *parentpc)
{
    JS_ASSERT(inlining());

    CompileStatus status = checkAnalysis(script);
    if (status != Compile_Okay)
        return status;

    if (!ssa.addInlineFrame(script, depth, parent, parentpc))
        return Compile_Error;

    uint32_t index = ssa.iterFrame(ssa.numFrames() - 1).index;
    return scanInlineCalls(index, depth);
}

CompileStatus
mjit::Compiler::scanInlineCalls(uint32_t index, uint32_t depth)
{
    /* Maximum number of calls we will inline at the same site. */
    static const uint32_t INLINE_SITE_LIMIT = 5;

    JS_ASSERT(inlining() && globalObj);

    /* Not inlining yet from 'new' scripts. */
    if (isConstructing)
        return Compile_Okay;

    JSScript *script = ssa.getFrame(index).script;
    ScriptAnalysis *analysis = script->analysis();

    /* Don't inline from functions which could have a non-global scope object. */
    if (!script->hasGlobal() ||
        script->global() != globalObj ||
        (script->function() && script->function()->getParent() != globalObj) ||
        (script->function() && script->function()->isHeavyweight()) ||
        script->isActiveEval) {
        return Compile_Okay;
    }

    uint32_t nextOffset = 0;
    uint32_t lastOffset = script->length;

    if (index == CrossScriptSSA::OUTER_FRAME) {
        nextOffset = outerChunk.begin;
        lastOffset = outerChunk.end;
    }

    while (nextOffset < lastOffset) {
        uint32_t offset = nextOffset;
        jsbytecode *pc = script->code + offset;
        nextOffset = offset + GetBytecodeLength(pc);

        Bytecode *code = analysis->maybeCode(pc);
        if (!code)
            continue;

        /* :XXX: Not yet inlining 'new' calls. */
        if (JSOp(*pc) != JSOP_CALL)
            continue;

        /* Not inlining at monitored call sites or those with type barriers. */
        if (code->monitoredTypes || code->monitoredTypesReturn || analysis->typeBarriers(cx, pc) != NULL)
            continue;

        uint32_t argc = GET_ARGC(pc);
        types::TypeSet *calleeTypes = analysis->poppedTypes(pc, argc + 1);

        if (calleeTypes->getKnownTypeTag(cx) != JSVAL_TYPE_OBJECT)
            continue;

        if (calleeTypes->getObjectCount() >= INLINE_SITE_LIMIT)
            continue;

        /*
         * Compute the maximum height we can grow the stack for inlined frames.
         * We always reserve space for loop temporaries, for an extra stack
         * frame pushed when making a call from the deepest inlined frame, and
         * for the temporary slot used by type barriers.
         */
        uint32_t stackLimit = outerScript->nslots + StackSpace::STACK_JIT_EXTRA
            - VALUES_PER_STACK_FRAME - FrameState::TEMPORARY_LIMIT - 1;

        /* Compute the depth of any frames inlined at this site. */
        uint32_t nextDepth = depth + VALUES_PER_STACK_FRAME + script->nfixed + code->stackDepth;

        /*
         * Scan each of the possible callees for other conditions precluding
         * inlining. We only inline at a call site if all callees are inlineable.
         */
        unsigned count = calleeTypes->getObjectCount();
        bool okay = true;
        for (unsigned i = 0; i < count; i++) {
            if (calleeTypes->getTypeObject(i) != NULL) {
                okay = false;
                break;
            }

            JSObject *obj = calleeTypes->getSingleObject(i);
            if (!obj)
                continue;

            if (!obj->isFunction()) {
                okay = false;
                break;
            }

            JSFunction *fun = obj->toFunction();
            if (!fun->isInterpreted()) {
                okay = false;
                break;
            }
            RootedScript script(cx, fun->script());

            /*
             * Don't inline calls to scripts which haven't been analyzed.
             * We need to analyze the inlined scripts to compile them, and
             * doing so can change type information we have queried already
             * in making inlining decisions.
             */
            if (!script->hasAnalysis() || !script->analysis()->ranInference()) {
                okay = false;
                break;
            }

            /* See bug 768313. */
            if (script->hasScriptCounts != outerScript->hasScriptCounts) {
                okay = false;
                break;
            }

            /*
             * The outer and inner scripts must have the same scope. This only
             * allows us to inline calls between non-inner functions. Also
             * check for consistent strictness between the functions.
             */
            if (!globalObj ||
                fun->getParent() != globalObj ||
                outerScript->strictModeCode != script->strictModeCode) {
                okay = false;
                break;
            }

            /* We can't cope with inlining recursive functions yet. */
            uint32_t nindex = index;
            while (nindex != CrossScriptSSA::INVALID_FRAME) {
                if (ssa.getFrame(nindex).script == script)
                    okay = false;
                nindex = ssa.getFrame(nindex).parent;
            }
            if (!okay)
                break;

            /* Watch for excessively deep nesting of inlined frames. */
            if (nextDepth + script->nslots >= stackLimit) {
                okay = false;
                break;
            }

            if (!script->types || !script->types->hasScope()) {
                okay = false;
                break;
            }

            CompileStatus status = checkAnalysis(script);
            if (status != Compile_Okay)
                return status;

            if (!script->analysis()->inlineable(argc)) {
                okay = false;
                break;
            }

            if (types::TypeSet::HasObjectFlags(cx, fun->getType(cx),
                                               types::OBJECT_FLAG_UNINLINEABLE)) {
                okay = false;
                break;
            }

            /*
             * Don't inline scripts which use 'this' if it is possible they
             * could be called with a 'this' value requiring wrapping. During
             * inlining we do not want to modify frame entries belonging to the
             * caller.
             */
            if (script->analysis()->usesThisValue() &&
                types::TypeScript::ThisTypes(script)->getKnownTypeTag(cx) != JSVAL_TYPE_OBJECT) {
                okay = false;
                break;
            }
        }
        if (!okay)
            continue;

        calleeTypes->addFreeze(cx);

        /*
         * Add the inline frames to the cross script SSA. We will pick these
         * back up when compiling the call site.
         */
        for (unsigned i = 0; i < count; i++) {
            JSObject *obj = calleeTypes->getSingleObject(i);
            if (!obj)
                continue;

            JSFunction *fun = obj->toFunction();
            RootedScript script(cx, fun->script());

            CompileStatus status = addInlineFrame(script, nextDepth, index, pc);
            if (status != Compile_Okay)
                return status;
        }
    }

    return Compile_Okay;
}

CompileStatus
mjit::Compiler::pushActiveFrame(JSScript *script, uint32_t argc)
{
    if (cx->runtime->profilingScripts && !script->hasScriptCounts)
        script->initScriptCounts(cx);

    ActiveFrame *newa = OffTheBooks::new_<ActiveFrame>(cx);
    if (!newa) {
        js_ReportOutOfMemory(cx);
        return Compile_Error;
    }

    newa->parent = a;
    if (a)
        newa->parentPC = PC;
    newa->script = script;
    newa->mainCodeStart = masm.size();
    newa->stubCodeStart = stubcc.size();

    if (outer) {
        newa->inlineIndex = uint32_t(inlineFrames.length());
        inlineFrames.append(newa);
    } else {
        newa->inlineIndex = CrossScriptSSA::OUTER_FRAME;
        outer = newa;
    }
    JS_ASSERT(ssa.getFrame(newa->inlineIndex).script == script);

    newa->inlinePCOffset = ssa.frameLength(newa->inlineIndex);

    ScriptAnalysis *newAnalysis = script->analysis();

#ifdef JS_METHODJIT_SPEW
    if (cx->typeInferenceEnabled() && IsJaegerSpewChannelActive(JSpew_Regalloc)) {
        unsigned nargs = script->function() ? script->function()->nargs : 0;
        for (unsigned i = 0; i < nargs; i++) {
            uint32_t slot = ArgSlot(i);
            if (!newAnalysis->slotEscapes(slot)) {
                JaegerSpew(JSpew_Regalloc, "Argument %u:", i);
                newAnalysis->liveness(slot).print();
            }
        }
        for (unsigned i = 0; i < script->nfixed; i++) {
            uint32_t slot = LocalSlot(script, i);
            if (!newAnalysis->slotEscapes(slot)) {
                JaegerSpew(JSpew_Regalloc, "Local %u:", i);
                newAnalysis->liveness(slot).print();
            }
        }
    }
#endif

    if (!frame.pushActiveFrame(script, argc)) {
        js_ReportOutOfMemory(cx);
        return Compile_Error;
    }

    newa->jumpMap = (Label *)OffTheBooks::malloc_(sizeof(Label) * script->length);
    if (!newa->jumpMap) {
        js_ReportOutOfMemory(cx);
        return Compile_Error;
    }
#ifdef DEBUG
    for (uint32_t i = 0; i < script->length; i++)
        newa->jumpMap[i] = Label();
#endif

    if (cx->typeInferenceEnabled()) {
        CompileStatus status = prepareInferenceTypes(script, newa);
        if (status != Compile_Okay)
            return status;
    }

    this->script = script;
    this->analysis = newAnalysis;
    this->PC = script->code;
    this->a = newa;

    return Compile_Okay;
}

void
mjit::Compiler::popActiveFrame()
{
    JS_ASSERT(a->parent);
    a->mainCodeEnd = masm.size();
    a->stubCodeEnd = stubcc.size();
    this->PC = a->parentPC;
    this->a = (ActiveFrame *) a->parent;
    this->script = a->script;
    this->analysis = this->script->analysis();

    frame.popActiveFrame();
}

#define CHECK_STATUS(expr)                                           \
    JS_BEGIN_MACRO                                                   \
        CompileStatus status_ = (expr);                              \
        if (status_ != Compile_Okay) {                               \
            if (oomInVector || masm.oom() || stubcc.masm.oom())      \
                js_ReportOutOfMemory(cx);                            \
            return status_;                                          \
        }                                                            \
    JS_END_MACRO

CompileStatus
mjit::Compiler::performCompilation()
{
    JaegerSpew(JSpew_Scripts,
               "compiling script (file \"%s\") (line \"%d\") (length \"%d\") (chunk \"%d\")\n",
               outerScript->filename, outerScript->lineno, outerScript->length, chunkIndex);

    if (inlining()) {
        JaegerSpew(JSpew_Inlining,
                   "inlining calls in script (file \"%s\") (line \"%d\")\n",
                   outerScript->filename, outerScript->lineno);
    }

#ifdef JS_METHODJIT_SPEW
    Profiler prof;
    prof.start();
#endif

#ifdef JS_METHODJIT
    outerScript->debugMode = debugMode();
#endif

    JS_ASSERT(cx->compartment->activeInference);

    {
        types::AutoEnterCompilation enter(cx, outerScript, isConstructing, chunkIndex);

        CHECK_STATUS(checkAnalysis(outerScript));
        if (inlining())
            CHECK_STATUS(scanInlineCalls(CrossScriptSSA::OUTER_FRAME, 0));
        CHECK_STATUS(pushActiveFrame(outerScript, 0));

        if (outerScript->hasScriptCounts || Probes::wantNativeAddressInfo(cx)) {
            size_t length = ssa.frameLength(ssa.numFrames() - 1);
            pcLengths = (PCLengthEntry *) OffTheBooks::calloc_(sizeof(pcLengths[0]) * length);
            if (!pcLengths)
                return Compile_Error;
        }

        if (chunkIndex == 0)
            CHECK_STATUS(generatePrologue());
        CHECK_STATUS(generateMethod());
        if (outerJIT() && chunkIndex == outerJIT()->nchunks - 1)
            CHECK_STATUS(generateEpilogue());
        CHECK_STATUS(finishThisUp());
    }

#ifdef JS_METHODJIT_SPEW
    prof.stop();
    JaegerSpew(JSpew_Prof, "compilation took %d us\n", prof.time_us());
#endif

    JaegerSpew(JSpew_Scripts, "successfully compiled (code \"%p\") (size \"%u\")\n",
               outerChunkRef().chunk->code.m_code.executableAddress(),
               unsigned(outerChunkRef().chunk->code.m_size));

    return Compile_Okay;
}

#undef CHECK_STATUS

mjit::JSActiveFrame::JSActiveFrame()
    : parent(NULL), parentPC(NULL), script(NULL), inlineIndex(UINT32_MAX)
{
}

mjit::Compiler::ActiveFrame::ActiveFrame(JSContext *cx)
    : jumpMap(NULL),
      varTypes(NULL), needReturnValue(false),
      syncReturnValue(false), returnValueDouble(false), returnSet(false),
      returnEntry(NULL), returnJumps(NULL), exitState(NULL)
{}

mjit::Compiler::ActiveFrame::~ActiveFrame()
{
    js::Foreground::free_(jumpMap);
    if (varTypes)
        js::Foreground::free_(varTypes);
}

mjit::Compiler::~Compiler()
{
    if (outer)
        cx->delete_(outer);
    for (unsigned i = 0; i < inlineFrames.length(); i++)
        cx->delete_(inlineFrames[i]);
    while (loop) {
        LoopState *nloop = loop->outer;
        cx->delete_(loop);
        loop = nloop;
    }
}

CompileStatus
mjit::Compiler::prepareInferenceTypes(JSScript *script, ActiveFrame *a)
{
    /*
     * During our walk of the script, we need to preserve the invariant that at
     * join points the in memory type tag is always in sync with the known type
     * tag of the variable's SSA value at that join point. In particular, SSA
     * values inferred as (int|double) must in fact be doubles, stored either
     * in floating point registers or in memory. There is an exception for
     * locals whose value is currently dead, whose type might not be synced.
     *
     * To ensure this, we need to know the SSA values for each variable at each
     * join point, which the SSA analysis does not store explicitly. These can
     * be recovered, though. During the forward walk, the SSA value of a var
     * (and its associated type set) change only when we see an explicit assign
     * to the var or get to a join point with a phi node for that var. So we
     * can duplicate the effects of that walk here by watching for writes to
     * vars (updateVarTypes) and new phi nodes at join points.
     *
     * When we get to a branch and need to know a variable's value at the
     * branch target, we know it will either be a phi node at the target or
     * the variable's current value, as no phi node is created at the target
     * only if a variable has the same value on all incoming edges.
     */

    a->varTypes = (VarType *)
        OffTheBooks::calloc_(TotalSlots(script) * sizeof(VarType));
    if (!a->varTypes) {
        js_ReportOutOfMemory(cx);
        return Compile_Error;
    }

    for (uint32_t slot = ArgSlot(0); slot < TotalSlots(script); slot++) {
        VarType &vt = a->varTypes[slot];
        vt.setTypes(types::TypeScript::SlotTypes(script, slot));
    }

    return Compile_Okay;
}

/*
 * Number of times a script must be called or have back edges taken before we
 * run it in the methodjit. We wait longer if type inference is enabled, to
 * allow more gathering of type information and less recompilation.
 */
static const size_t USES_BEFORE_COMPILE       = 16;
static const size_t INFER_USES_BEFORE_COMPILE = 43;

/* Target maximum size, in bytecode length, for a compiled chunk of a script. */
static uint32_t CHUNK_LIMIT = 1500;

void
mjit::SetChunkLimit(uint32_t limit)
{
    if (limit)
        CHUNK_LIMIT = limit;
}

JITScript *
MakeJITScript(JSContext *cx, JSScript *script)
{
    if (!script->ensureRanAnalysis(cx, NULL))
        return NULL;

    ScriptAnalysis *analysis = script->analysis();

    Vector<ChunkDescriptor> chunks(cx);
    Vector<CrossChunkEdge> edges(cx);

    if (script->length < CHUNK_LIMIT || !cx->typeInferenceEnabled()) {
        ChunkDescriptor desc;
        desc.begin = 0;
        desc.end = script->length;
        if (!chunks.append(desc))
            return NULL;
    } else {
        if (!script->ensureRanInference(cx))
            return NULL;

        /* Outgoing edges within the current chunk. */
        Vector<CrossChunkEdge> currentEdges(cx);
        uint32_t chunkStart = 0;

        unsigned offset, nextOffset = 0;
        while (nextOffset < script->length) {
            offset = nextOffset;

            jsbytecode *pc = script->code + offset;
            JSOp op = JSOp(*pc);

            nextOffset = offset + GetBytecodeLength(pc);

            Bytecode *code = analysis->maybeCode(offset);
            if (!code)
                continue;

            /* Whether this should be the last opcode in the chunk. */
            bool finishChunk = false;

            /* Keep going, override finishChunk. */
            bool preserveChunk = false;

            /*
             * Add an edge for opcodes which perform a branch. Skip LABEL ops,
             * which do not actually branch. XXX LABEL should not be JOF_JUMP.
             */
            uint32_t type = JOF_TYPE(js_CodeSpec[op].format);
            if (type == JOF_JUMP && op != JSOP_LABEL) {
                CrossChunkEdge edge;
                edge.source = offset;
                edge.target = FollowBranch(cx, script, pc - script->code);
                if (edge.target < offset) {
                    /* Always end chunks after loop back edges. */
                    finishChunk = true;
                    if (edge.target < chunkStart) {
                        analysis->getCode(edge.target).safePoint = true;
                        if (!edges.append(edge))
                            return NULL;
                    }
                } else if (edge.target == nextOffset) {
                    /*
                     * Override finishChunk for bytecodes which directly
                     * jump to their fallthrough opcode ('if (x) {}'). This
                     * creates two CFG edges with the same source/target, which
                     * will confuse the compiler's edge patching code.
                     */
                    preserveChunk = true;
                } else {
                    if (!currentEdges.append(edge))
                        return NULL;
                }
            }

            if (op == JSOP_TABLESWITCH) {
                jsbytecode *pc2 = pc;
                unsigned defaultOffset = offset + GET_JUMP_OFFSET(pc);
                pc2 += JUMP_OFFSET_LEN;
                int32_t low = GET_JUMP_OFFSET(pc2);
                pc2 += JUMP_OFFSET_LEN;
                int32_t high = GET_JUMP_OFFSET(pc2);
                pc2 += JUMP_OFFSET_LEN;

                CrossChunkEdge edge;
                edge.source = offset;
                edge.target = defaultOffset;
                if (!currentEdges.append(edge))
                    return NULL;

                for (int32_t i = low; i <= high; i++) {
                    unsigned targetOffset = offset + GET_JUMP_OFFSET(pc2);
                    if (targetOffset != offset) {
                        /*
                         * This can end up inserting duplicate edges, all but
                         * the first of which will be ignored.
                         */
                        CrossChunkEdge edge;
                        edge.source = offset;
                        edge.target = targetOffset;
                        if (!currentEdges.append(edge))
                            return NULL;
                    }
                    pc2 += JUMP_OFFSET_LEN;
                }
            }

            if (op == JSOP_LOOKUPSWITCH) {
                unsigned defaultOffset = offset + GET_JUMP_OFFSET(pc);
                jsbytecode *pc2 = pc + JUMP_OFFSET_LEN;
                unsigned npairs = GET_UINT16(pc2);
                pc2 += UINT16_LEN;

                CrossChunkEdge edge;
                edge.source = offset;
                edge.target = defaultOffset;
                if (!currentEdges.append(edge))
                    return NULL;

                while (npairs) {
                    pc2 += UINT32_INDEX_LEN;
                    unsigned targetOffset = offset + GET_JUMP_OFFSET(pc2);
                    CrossChunkEdge edge;
                    edge.source = offset;
                    edge.target = targetOffset;
                    if (!currentEdges.append(edge))
                        return NULL;
                    pc2 += JUMP_OFFSET_LEN;
                    npairs--;
                }
            }

            if (unsigned(offset - chunkStart) > CHUNK_LIMIT)
                finishChunk = true;

            if (nextOffset >= script->length || !analysis->maybeCode(nextOffset)) {
                /* Ensure that chunks do not start on unreachable opcodes. */
                preserveChunk = true;
            } else {
                /*
                 * Start new chunks at the opcode before each loop head.
                 * This ensures that the initial goto for loops is included in
                 * the same chunk as the loop itself.
                 */
                jsbytecode *nextpc = script->code + nextOffset;

                /*
                 * Don't insert a chunk boundary in the middle of two opcodes
                 * which may be fused together.
                 */
                switch (JSOp(*nextpc)) {
                  case JSOP_POP:
                  case JSOP_IFNE:
                  case JSOP_IFEQ:
                    preserveChunk = true;
                    break;
                  default:
                    break;
                }

                uint32_t afterOffset = nextOffset + GetBytecodeLength(nextpc);
                if (afterOffset < script->length) {
                    if (analysis->maybeCode(afterOffset) &&
                        JSOp(script->code[afterOffset]) == JSOP_LOOPHEAD &&
                        analysis->getLoop(afterOffset))
                    {
                        finishChunk = true;
                    }
                }
            }

            if (finishChunk && !preserveChunk) {
                ChunkDescriptor desc;
                desc.begin = chunkStart;
                desc.end = nextOffset;
                if (!chunks.append(desc))
                    return NULL;

                /* Add an edge for fallthrough from this chunk to the next one. */
                if (!BytecodeNoFallThrough(op)) {
                    CrossChunkEdge edge;
                    edge.source = offset;
                    edge.target = nextOffset;
                    analysis->getCode(edge.target).safePoint = true;
                    if (!edges.append(edge))
                        return NULL;
                }

                chunkStart = nextOffset;
                for (unsigned i = 0; i < currentEdges.length(); i++) {
                    const CrossChunkEdge &edge = currentEdges[i];
                    if (edge.target >= nextOffset) {
                        analysis->getCode(edge.target).safePoint = true;
                        if (!edges.append(edge))
                            return NULL;
                    }
                }
                currentEdges.clear();
            }
        }

        if (chunkStart != script->length) {
            ChunkDescriptor desc;
            desc.begin = chunkStart;
            desc.end = script->length;
            if (!chunks.append(desc))
                return NULL;
        }
    }

    size_t dataSize = sizeof(JITScript)
        + (chunks.length() * sizeof(ChunkDescriptor))
        + (edges.length() * sizeof(CrossChunkEdge));
    uint8_t *cursor = (uint8_t *) OffTheBooks::calloc_(dataSize);
    if (!cursor)
        return NULL;

    JITScript *jit = (JITScript *) cursor;
    cursor += sizeof(JITScript);

    jit->script = script;
    JS_INIT_CLIST(&jit->callers);

    jit->nchunks = chunks.length();
    for (unsigned i = 0; i < chunks.length(); i++) {
        const ChunkDescriptor &a = chunks[i];
        ChunkDescriptor &b = jit->chunkDescriptor(i);
        b.begin = a.begin;
        b.end = a.end;

        if (chunks.length() == 1) {
            /* Seed the chunk's count so it is immediately compiled. */
            b.counter = INFER_USES_BEFORE_COMPILE;
        }
    }

    if (edges.empty())
        return jit;

    jit->nedges = edges.length();
    CrossChunkEdge *jitEdges = jit->edges();
    for (unsigned i = 0; i < edges.length(); i++) {
        const CrossChunkEdge &a = edges[i];
        CrossChunkEdge &b = jitEdges[i];
        b.source = a.source;
        b.target = a.target;
    }

    /* Generate a pool with all cross chunk shims, and set shimLabel for each edge. */
    Assembler masm;
    for (unsigned i = 0; i < jit->nedges; i++) {
        jsbytecode *pc = script->code + jitEdges[i].target;
        jitEdges[i].shimLabel = (void *) masm.distanceOf(masm.label());
        masm.move(JSC::MacroAssembler::ImmPtr(&jitEdges[i]), Registers::ArgReg1);
        masm.fallibleVMCall(true, JS_FUNC_TO_DATA_PTR(void *, stubs::CrossChunkShim),
                            pc, NULL, script->nfixed + analysis->getCode(pc).stackDepth);
    }
    LinkerHelper linker(masm, JSC::METHOD_CODE);
    JSC::ExecutablePool *ep = linker.init(cx);
    if (!ep)
        return NULL;
    jit->shimPool = ep;

    masm.finalize(linker);
    uint8_t *shimCode = (uint8_t *) linker.finalizeCodeAddendum().executableAddress();

    JS_ALWAYS_TRUE(linker.verifyRange(JSC::JITCode(shimCode, masm.size())));

    JaegerSpew(JSpew_PICs, "generated SHIM POOL stub %p (%lu bytes)\n",
               shimCode, (unsigned long)masm.size());

    for (unsigned i = 0; i < jit->nedges; i++) {
        CrossChunkEdge &edge = jitEdges[i];
        edge.shimLabel = shimCode + (size_t) edge.shimLabel;
    }

    return jit;
}

static inline bool
IonGetsFirstChance(JSContext *cx, JSScript *script, CompileRequest request)
{
#ifdef JS_ION
    if (!ion::IsEnabled(cx))
        return false;

    // If the script is not hot, use JM. recompileCheckHelper will insert a check
    // to trigger a recompile when the script becomes hot.
    if (script->getUseCount() < ion::js_IonOptions.usesBeforeCompile)
        return false;

    // If we're called from JM, use JM to avoid slow JM -> Ion calls.
    if (request == CompileRequest_JIT)
        return false;

    // If there's no way this script is going to be Ion compiled, let JM take over.
    if (!script->canIonCompile())
        return false;

    // If we cannot enter Ion because bailouts are expected, let JM take over.
    if (script->hasIonScript() && script->ion->bailoutExpected())
        return false;

    return true;
#endif
    return false;
}

CompileStatus
mjit::CanMethodJIT(JSContext *cx, JSScript *script, jsbytecode *pc,
                   bool construct, CompileRequest request)
{
  restart:
    if (!cx->methodJitEnabled)
        return Compile_Abort;

    if (IonGetsFirstChance(cx, script, request))
        return Compile_Skipped;

    if (script->hasJITInfo()) {
        JSScript::JITScriptHandle *jith = script->jitHandle(construct, cx->compartment->needsBarrier());
        if (jith->isUnjittable())
            return Compile_Abort;
    }

    if (!cx->hasRunOption(JSOPTION_METHODJIT_ALWAYS) &&
        (cx->typeInferenceEnabled()
         ? script->incUseCount() <= INFER_USES_BEFORE_COMPILE
         : script->incUseCount() <= USES_BEFORE_COMPILE))
    {
        return Compile_Skipped;
    }

    if (!cx->runtime->getJaegerRuntime(cx))
        return Compile_Error;

    // Ensure that constructors have at least one slot.
    if (construct && !script->nslots)
        script->nslots++;

    uint64_t gcNumber = cx->runtime->gcNumber;

    if (!script->ensureHasJITInfo(cx))
        return Compile_Error;

    JSScript::JITScriptHandle *jith = script->jitHandle(construct, cx->compartment->needsBarrier());

    JITScript *jit;
    if (jith->isEmpty()) {
        jit = MakeJITScript(cx, script);
        if (!jit)
            return Compile_Error;

        // Script analysis can trigger GC, watch in case needsBarrier() changed.
        if (gcNumber != cx->runtime->gcNumber) {
            FreeOp *fop = cx->runtime->defaultFreeOp();
            jit->destroy(fop);
            fop->free_(jit);
            goto restart;
        }

        jith->setValid(jit);
    } else {
        jit = jith->getValid();
    }

    unsigned chunkIndex = jit->chunkIndex(pc);
    ChunkDescriptor &desc = jit->chunkDescriptor(chunkIndex);

    if (desc.chunk)
        return Compile_Okay;

    if (!cx->hasRunOption(JSOPTION_METHODJIT_ALWAYS) &&
        ++desc.counter <= INFER_USES_BEFORE_COMPILE)
    {
        return Compile_Skipped;
    }

    CompileStatus status;
    {
        types::AutoEnterTypeInference enter(cx, true);

        Compiler cc(cx, script, chunkIndex, construct);
        status = cc.compile();
    }

    if (status == Compile_Okay) {
        /*
         * Compiling a script can occasionally trigger its own recompilation,
         * so go back through the compilation logic.
         */
        goto restart;
    }

    /* Non-OOM errors should have an associated exception. */
    JS_ASSERT_IF(status == Compile_Error,
                 cx->isExceptionPending() || cx->runtime->hadOutOfMemory);

    return status;
}

CompileStatus
mjit::Compiler::generatePrologue()
{
    fastEntryLabel = masm.label();

    /*
     * If there is no function, then this can only be called via JaegerShot(),
     * which expects an existing frame to be initialized like the interpreter.
     */
    if (script->function()) {
        Jump j = masm.jump();

        /*
         * Entry point #2: The caller has partially constructed a frame, and
         * either argc >= nargs or the arity check has corrected the frame.
         */
        fastEntryLabel = masm.label();

        /* Store this early on so slow paths can access it. */
        masm.storePtr(ImmPtr(script->function()),
                      Address(JSFrameReg, StackFrame::offsetOfExec()));

        {
            /*
             * Entry point #3: The caller has partially constructed a frame,
             * but argc might be != nargs, so an arity check might be called.
             *
             * This loops back to entry point #2.
             */
            arityLabel = stubcc.masm.label();

            Jump argMatch = stubcc.masm.branch32(Assembler::Equal, JSParamReg_Argc,
                                                 Imm32(script->function()->nargs));

            if (JSParamReg_Argc != Registers::ArgReg1)
                stubcc.masm.move(JSParamReg_Argc, Registers::ArgReg1);

            /* Slow path - call the arity check function. Returns new fp. */
            stubcc.masm.storePtr(ImmPtr(script->function()),
                                 Address(JSFrameReg, StackFrame::offsetOfExec()));
            OOL_STUBCALL(stubs::FixupArity, REJOIN_NONE);
            stubcc.masm.move(Registers::ReturnReg, JSFrameReg);
            argMatch.linkTo(stubcc.masm.label(), &stubcc.masm);

            argsCheckLabel = stubcc.masm.label();

            /* Type check the arguments as well. */
            if (cx->typeInferenceEnabled()) {
#ifdef JS_MONOIC
                this->argsCheckJump = stubcc.masm.jump();
                this->argsCheckStub = stubcc.masm.label();
                this->argsCheckJump.linkTo(this->argsCheckStub, &stubcc.masm);
#endif
                stubcc.masm.storePtr(ImmPtr(script->function()),
                                     Address(JSFrameReg, StackFrame::offsetOfExec()));
                OOL_STUBCALL(stubs::CheckArgumentTypes, REJOIN_CHECK_ARGUMENTS);
#ifdef JS_MONOIC
                this->argsCheckFallthrough = stubcc.masm.label();
#endif
            }

            stubcc.crossJump(stubcc.masm.jump(), fastEntryLabel);
        }

        /*
         * Guard that there is enough stack space. Note we reserve space for
         * any inline frames we end up generating, or a callee's stack frame
         * we write to before the callee checks the stack.
         */
        uint32_t nvals = VALUES_PER_STACK_FRAME + script->nslots + StackSpace::STACK_JIT_EXTRA;
        masm.addPtr(Imm32(nvals * sizeof(Value)), JSFrameReg, Registers::ReturnReg);
        Jump stackCheck = masm.branchPtr(Assembler::AboveOrEqual, Registers::ReturnReg,
                                         FrameAddress(offsetof(VMFrame, stackLimit)));

        /*
         * If the stack check fails then we need to either commit more of the
         * reserved stack space or throw an error. Specify that the number of
         * local slots is 0 (instead of the default script->nfixed) since the
         * range [fp->slots(), fp->base()) may not be commited. (The calling
         * contract requires only that the caller has reserved space for fp.)
         */
        {
            stubcc.linkExitDirect(stackCheck, stubcc.masm.label());
            OOL_STUBCALL(stubs::HitStackQuota, REJOIN_NONE);
            stubcc.crossJump(stubcc.masm.jump(), masm.label());
        }

        markUndefinedLocals();

        /*
         * Load the scope chain into the frame if it will be needed by NAME
         * opcodes or by the nesting prologue below. The scope chain is always
         * set for global and eval frames, and will have been set by
         * HeavyweightFunctionPrologue for heavyweight function frames.
         */
        if (!script->function()->isHeavyweight() &&
            (analysis->usesScopeChain() || script->nesting()))
        {
            RegisterID t0 = Registers::ReturnReg;
            Jump hasScope = masm.branchTest32(Assembler::NonZero,
                                              FrameFlagsAddress(), Imm32(StackFrame::HAS_SCOPECHAIN));
            masm.loadPayload(Address(JSFrameReg, StackFrame::offsetOfCallee(script->function())), t0);
            masm.loadPtr(Address(t0, JSFunction::offsetOfEnvironment()), t0);
            masm.storePtr(t0, Address(JSFrameReg, StackFrame::offsetOfScopeChain()));
            hasScope.linkTo(masm.label(), &masm);
        }

        /*
         * When 'arguments' is used in the script, it may be optimized away
         * which involves reading from the stack frame directly, including
         * fp->u.nactual. fp->u.nactual is only set when numActual != numFormal,
         * so store 'fp->u.nactual = numFormal' when there is no over/underflow.
         */
        if (script->argumentsHasVarBinding()) {
            Jump hasArgs = masm.branchTest32(Assembler::NonZero, FrameFlagsAddress(),
                                             Imm32(StackFrame::UNDERFLOW_ARGS |
                                                   StackFrame::OVERFLOW_ARGS));
            masm.storePtr(ImmPtr((void *)(size_t) script->function()->nargs),
                          Address(JSFrameReg, StackFrame::offsetOfNumActual()));
            hasArgs.linkTo(masm.label(), &masm);
        }

        j.linkTo(masm.label(), &masm);
    }

    if (cx->typeInferenceEnabled()) {
#ifdef DEBUG
        if (script->function()) {
            prepareStubCall(Uses(0));
            INLINE_STUBCALL(stubs::AssertArgumentTypes, REJOIN_NONE);
        }
#endif
        ensureDoubleArguments();
    }

    /* Inline StackFrame::prologue. */
    if (script->isActiveEval && script->strictModeCode) {
        prepareStubCall(Uses(0));
        INLINE_STUBCALL(stubs::StrictEvalPrologue, REJOIN_EVAL_PROLOGUE);
    } else if (script->function()) {
        if (script->function()->isHeavyweight()) {
            prepareStubCall(Uses(0));
            INLINE_STUBCALL(stubs::HeavyweightFunctionPrologue, REJOIN_FUNCTION_PROLOGUE);
        } else if (types::TypeScriptNesting *nesting = script->nesting()) {
            /*
             * Inline the common case for the nesting prologue: the
             * function is a non-heavyweight inner function with no
             * children of its own. We ensure during inference that the
             * outer function does not add scope objects for 'let' or
             * 'with', so that the frame's scope chain will be
             * the parent's call object, and if it differs from the
             * parent's current activation then the parent is reentrant.
             */
            JSScript *parent = nesting->parent;
            JS_ASSERT(parent);
            JS_ASSERT_IF(parent->hasAnalysis() && parent->analysis()->ranBytecode(),
                         !parent->analysis()->addsScopeObjects());

            RegisterID t0 = Registers::ReturnReg;
            masm.move(ImmPtr(&parent->nesting()->activeCall), t0);
            masm.loadPtr(Address(t0), t0);

            Address scopeChain(JSFrameReg, StackFrame::offsetOfScopeChain());
            Jump mismatch = masm.branchPtr(Assembler::NotEqual, t0, scopeChain);
            masm.add32(Imm32(1), AbsoluteAddress(&nesting->activeFrames));

            masm.load32(FrameFlagsAddress(), t0);
            masm.or32(Imm32(StackFrame::HAS_NESTING), t0);
            masm.store32(t0, FrameFlagsAddress());

            stubcc.linkExitDirect(mismatch, stubcc.masm.label());
            OOL_STUBCALL(stubs::TypeNestingPrologue, REJOIN_FUNCTION_PROLOGUE);
            stubcc.crossJump(stubcc.masm.jump(), masm.label());
        }

        if (isConstructing) {
            if (!constructThis())
                return Compile_Error;
        }
    }

    CompileStatus status = methodEntryHelper();
    if (status == Compile_Okay)
        recompileCheckHelper();

    return status;
}

void
mjit::Compiler::ensureDoubleArguments()
{
    /* Convert integer arguments which were inferred as (int|double) to doubles. */
    for (uint32_t i = 0; script->function() && i < script->function()->nargs; i++) {
        uint32_t slot = ArgSlot(i);
        if (a->varTypes[slot].getTypeTag(cx) == JSVAL_TYPE_DOUBLE && analysis->trackSlot(slot))
            frame.ensureDouble(frame.getArg(i));
    }
}

void
mjit::Compiler::markUndefinedLocal(uint32_t offset, uint32_t i)
{
    uint32_t depth = ssa.getFrame(a->inlineIndex).depth;
    uint32_t slot = LocalSlot(script, i);
    Address local(JSFrameReg, sizeof(StackFrame) + (depth + i) * sizeof(Value));
    if (!cx->typeInferenceEnabled() || !analysis->trackSlot(slot)) {
        masm.storeValue(UndefinedValue(), local);
    } else {
        Lifetime *lifetime = analysis->liveness(slot).live(offset);
        if (lifetime)
            masm.storeValue(UndefinedValue(), local);
#ifdef DEBUG
        else
            masm.storeValue(ObjectValueCrashOnTouch(), local);
#endif
    }
}

void
mjit::Compiler::markUndefinedLocals()
{
    /*
     * Set locals to undefined. Skip locals which aren't closed and are known
     * to be defined before used,
     */
    for (uint32_t i = 0; i < script->nfixed; i++)
        markUndefinedLocal(0, i);

#ifdef DEBUG
    uint32_t depth = ssa.getFrame(a->inlineIndex).depth;
    for (uint32_t i = script->nfixed; i < script->nslots; i++) {
        Address local(JSFrameReg, sizeof(StackFrame) + (depth + i) * sizeof(Value));
        masm.storeValue(ObjectValueCrashOnTouch(), local);
    }
#endif
}

CompileStatus
mjit::Compiler::generateEpilogue()
{
    return Compile_Okay;
}

CompileStatus
mjit::Compiler::finishThisUp()
{
#ifdef JS_CPU_X64
    /* Generate trampolines to ensure that cross chunk edges are patchable. */
    for (unsigned i = 0; i < chunkEdges.length(); i++) {
        chunkEdges[i].sourceTrampoline = stubcc.masm.label();
        stubcc.masm.move(ImmPtr(NULL), Registers::ScratchReg);
        stubcc.masm.jump(Registers::ScratchReg);
    }
#endif

    RETURN_IF_OOM(Compile_Error);

    /*
     * Watch for reallocation of the global slots while we were in the middle
     * of compiling due to, e.g. standard class initialization.
     */
    if (globalSlots && globalObj->getRawSlots() != globalSlots)
        return Compile_Retry;

    /*
     * Watch for GCs which occurred during compilation. These may have
     * renumbered shapes baked into the jitcode.
     */
    if (cx->runtime->gcNumber != gcNumber)
        return Compile_Retry;

    /* The JIT will not have been cleared if no GC has occurred. */
    JITScript *jit = outerJIT();
    JS_ASSERT(jit != NULL);

    if (overflowICSpace) {
        JaegerSpew(JSpew_Scripts, "dumped a constant pool while generating an IC\n");
        return Compile_Abort;
    }

    a->mainCodeEnd = masm.size();
    a->stubCodeEnd = stubcc.size();

    for (size_t i = 0; i < branchPatches.length(); i++) {
        Label label = labelOf(branchPatches[i].pc, branchPatches[i].inlineIndex);
        branchPatches[i].jump.linkTo(label, &masm);
    }

#ifdef JS_CPU_ARM
    masm.forceFlushConstantPool();
    stubcc.masm.forceFlushConstantPool();
#endif
    JaegerSpew(JSpew_Insns, "## Fast code (masm) size = %lu, Slow code (stubcc) size = %lu.\n",
               (unsigned long) masm.size(), (unsigned long) stubcc.size());

    /* To make inlineDoubles and oolDoubles aligned to sizeof(double) bytes,
       MIPS adds extra sizeof(double) bytes to codeSize.  */
    size_t codeSize = masm.size() +
#if defined(JS_CPU_MIPS)
                      stubcc.size() + sizeof(double) +
#else
                      stubcc.size() +
#endif
                      (masm.numDoubles() * sizeof(double)) +
                      (stubcc.masm.numDoubles() * sizeof(double)) +
                      jumpTableEdges.length() * sizeof(void *);

    Vector<ChunkJumpTableEdge> chunkJumps(cx);
    if (!chunkJumps.reserve(jumpTableEdges.length()))
        return Compile_Error;

    JSC::ExecutableAllocator &execAlloc = cx->runtime->execAlloc();
    JSC::ExecutablePool *execPool;
    uint8_t *result = (uint8_t *)execAlloc.alloc(codeSize, &execPool, JSC::METHOD_CODE);
    if (!result) {
        js_ReportOutOfMemory(cx);
        return Compile_Error;
    }
    JS_ASSERT(execPool);
    JSC::ExecutableAllocator::makeWritable(result, codeSize);
    masm.executableCopy(result);
    stubcc.masm.executableCopy(result + masm.size());

    JSC::LinkBuffer fullCode(result, codeSize, JSC::METHOD_CODE);
    JSC::LinkBuffer stubCode(result + masm.size(), stubcc.size(), JSC::METHOD_CODE);

    JS_ASSERT(!loop);

    size_t nNmapLive = loopEntries.length();
    for (size_t i = outerChunk.begin; i < outerChunk.end; i++) {
        Bytecode *opinfo = analysis->maybeCode(i);
        if (opinfo && opinfo->safePoint)
            nNmapLive++;
    }

    /* Please keep in sync with JITChunk::sizeOfIncludingThis! */
    size_t dataSize = sizeof(JITChunk) +
                      sizeof(NativeMapEntry) * nNmapLive +
                      sizeof(InlineFrame) * inlineFrames.length() +
                      sizeof(CallSite) * callSites.length() +
                      sizeof(JSObject*) * rootedTemplates.length() +
                      sizeof(RegExpShared*) * rootedRegExps.length() +
#if defined JS_MONOIC
                      sizeof(ic::GetGlobalNameIC) * getGlobalNames.length() +
                      sizeof(ic::SetGlobalNameIC) * setGlobalNames.length() +
                      sizeof(ic::CallICInfo) * callICs.length() +
                      sizeof(ic::EqualityICInfo) * equalityICs.length() +
#endif
#if defined JS_POLYIC
                      sizeof(ic::PICInfo) * pics.length() +
                      sizeof(ic::GetElementIC) * getElemICs.length() +
                      sizeof(ic::SetElementIC) * setElemICs.length() +
#endif
                      0;

    uint8_t *cursor = (uint8_t *)OffTheBooks::calloc_(dataSize);
    if (!cursor) {
        execPool->release();
        js_ReportOutOfMemory(cx);
        return Compile_Error;
    }

    JITChunk *chunk = new(cursor) JITChunk;
    cursor += sizeof(JITChunk);

    JS_ASSERT(outerScript == script);

    chunk->code = JSC::MacroAssemblerCodeRef(result, execPool, masm.size() + stubcc.size());
    chunk->pcLengths = pcLengths;

    if (chunkIndex == 0) {
        jit->invokeEntry = result;
        if (script->function()) {
            jit->arityCheckEntry = stubCode.locationOf(arityLabel).executableAddress();
            jit->argsCheckEntry = stubCode.locationOf(argsCheckLabel).executableAddress();
            jit->fastEntry = fullCode.locationOf(fastEntryLabel).executableAddress();
        }
    }

    /*
     * WARNING: mics(), callICs() et al depend on the ordering of these
     * variable-length sections.  See JITChunk's declaration for details.
     */

    /* ICs can only refer to bytecodes in the outermost script, not inlined calls. */
    Label *jumpMap = a->jumpMap;

    /* Build the pc -> ncode mapping. */
    NativeMapEntry *jitNmap = (NativeMapEntry *)cursor;
    chunk->nNmapPairs = nNmapLive;
    cursor += sizeof(NativeMapEntry) * chunk->nNmapPairs;
    size_t ix = 0;
    if (chunk->nNmapPairs > 0) {
        for (size_t i = outerChunk.begin; i < outerChunk.end; i++) {
            Bytecode *opinfo = analysis->maybeCode(i);
            if (opinfo && opinfo->safePoint) {
                Label L = jumpMap[i];
                JS_ASSERT(L.isSet());
                jitNmap[ix].bcOff = i;
                jitNmap[ix].ncode = (uint8_t *)(result + masm.distanceOf(L));
                ix++;
            }
        }
        for (size_t i = 0; i < loopEntries.length(); i++) {
            /* Insert the entry at the right position. */
            const LoopEntry &entry = loopEntries[i];
            size_t j;
            for (j = 0; j < ix; j++) {
                if (jitNmap[j].bcOff > entry.pcOffset) {
                    memmove(jitNmap + j + 1, jitNmap + j, (ix - j) * sizeof(NativeMapEntry));
                    break;
                }
            }
            jitNmap[j].bcOff = entry.pcOffset;
            jitNmap[j].ncode = (uint8_t *) stubCode.locationOf(entry.label).executableAddress();
            ix++;
        }
    }
    JS_ASSERT(ix == chunk->nNmapPairs);

    /* Build the table of inlined frames. */
    InlineFrame *jitInlineFrames = (InlineFrame *)cursor;
    chunk->nInlineFrames = inlineFrames.length();
    cursor += sizeof(InlineFrame) * chunk->nInlineFrames;
    for (size_t i = 0; i < chunk->nInlineFrames; i++) {
        InlineFrame &to = jitInlineFrames[i];
        ActiveFrame *from = inlineFrames[i];
        if (from->parent != outer)
            to.parent = &jitInlineFrames[from->parent->inlineIndex];
        else
            to.parent = NULL;
        to.parentpc = from->parentPC;
        to.fun = from->script->function();
        to.depth = ssa.getFrame(from->inlineIndex).depth;
    }

    /* Build the table of call sites. */
    CallSite *jitCallSites = (CallSite *)cursor;
    chunk->nCallSites = callSites.length();
    cursor += sizeof(CallSite) * chunk->nCallSites;
    for (size_t i = 0; i < chunk->nCallSites; i++) {
        CallSite &to = jitCallSites[i];
        InternalCallSite &from = callSites[i];

        /* Patch stores of f.regs.inlined for stubs called from within inline frames. */
        if (cx->typeInferenceEnabled() &&
            from.rejoin != REJOIN_TRAP &&
            from.rejoin != REJOIN_SCRIPTED &&
            from.inlineIndex != UINT32_MAX) {
            if (from.ool)
                stubCode.patch(from.inlinePatch, &to);
            else
                fullCode.patch(from.inlinePatch, &to);
        }

        JSScript *script =
            (from.inlineIndex == UINT32_MAX) ? outerScript : inlineFrames[from.inlineIndex]->script;
        uint32_t codeOffset = from.ool
                            ? masm.size() + from.returnOffset
                            : from.returnOffset;
        to.initialize(codeOffset, from.inlineIndex, from.inlinepc - script->code, from.rejoin);

        /*
         * Patch stores of the base call's return address for InvariantFailure
         * calls. InvariantFailure will patch its own return address to this
         * pointer before triggering recompilation.
         */
        if (from.loopPatch.hasPatch)
            stubCode.patch(from.loopPatch.codePatch, result + codeOffset);
    }

    JSObject **jitRootedTemplates = (JSObject **)cursor;
    chunk->nRootedTemplates = rootedTemplates.length();
    cursor += sizeof(JSObject*) * chunk->nRootedTemplates;
    for (size_t i = 0; i < chunk->nRootedTemplates; i++)
        jitRootedTemplates[i] = rootedTemplates[i];

    RegExpShared **jitRootedRegExps = (RegExpShared **)cursor;
    chunk->nRootedRegExps = rootedRegExps.length();
    cursor += sizeof(RegExpShared*) * chunk->nRootedRegExps;
    for (size_t i = 0; i < chunk->nRootedRegExps; i++) {
        jitRootedRegExps[i] = rootedRegExps[i];
        jitRootedRegExps[i]->incRef();
    }

#if defined JS_MONOIC
    if (chunkIndex == 0 && script->function()) {
        JS_ASSERT(jit->argsCheckPool == NULL);
        if (cx->typeInferenceEnabled()) {
            jit->argsCheckStub = stubCode.locationOf(argsCheckStub);
            jit->argsCheckFallthrough = stubCode.locationOf(argsCheckFallthrough);
            jit->argsCheckJump = stubCode.locationOf(argsCheckJump);
        }
    }

    ic::GetGlobalNameIC *getGlobalNames_ = (ic::GetGlobalNameIC *)cursor;
    chunk->nGetGlobalNames = getGlobalNames.length();
    cursor += sizeof(ic::GetGlobalNameIC) * chunk->nGetGlobalNames;
    for (size_t i = 0; i < chunk->nGetGlobalNames; i++) {
        ic::GetGlobalNameIC &to = getGlobalNames_[i];
        GetGlobalNameICInfo &from = getGlobalNames[i];
        from.copyTo(to, fullCode, stubCode);

        int offset = fullCode.locationOf(from.load) - to.fastPathStart;
        to.loadStoreOffset = offset;
        JS_ASSERT(to.loadStoreOffset == offset);

        stubCode.patch(from.addrLabel, &to);
    }

    ic::SetGlobalNameIC *setGlobalNames_ = (ic::SetGlobalNameIC *)cursor;
    chunk->nSetGlobalNames = setGlobalNames.length();
    cursor += sizeof(ic::SetGlobalNameIC) * chunk->nSetGlobalNames;
    for (size_t i = 0; i < chunk->nSetGlobalNames; i++) {
        ic::SetGlobalNameIC &to = setGlobalNames_[i];
        SetGlobalNameICInfo &from = setGlobalNames[i];
        from.copyTo(to, fullCode, stubCode);
        to.slowPathStart = stubCode.locationOf(from.slowPathStart);

        int offset = fullCode.locationOf(from.store).labelAtOffset(0) -
                     to.fastPathStart;
        to.loadStoreOffset = offset;
        JS_ASSERT(to.loadStoreOffset == offset);

        to.objConst = from.objConst;
        to.shapeReg = from.shapeReg;
        to.objReg = from.objReg;
        to.vr = from.vr;

        offset = fullCode.locationOf(from.shapeGuardJump) -
                 to.fastPathStart;
        to.inlineShapeJump = offset;
        JS_ASSERT(to.inlineShapeJump == offset);

        offset = fullCode.locationOf(from.fastPathRejoin) -
                 to.fastPathStart;
        to.fastRejoinOffset = offset;
        JS_ASSERT(to.fastRejoinOffset == offset);

        stubCode.patch(from.addrLabel, &to);
    }

    ic::CallICInfo *jitCallICs = (ic::CallICInfo *)cursor;
    chunk->nCallICs = callICs.length();
    cursor += sizeof(ic::CallICInfo) * chunk->nCallICs;
    for (size_t i = 0; i < chunk->nCallICs; i++) {
        jitCallICs[i].funGuard = fullCode.locationOf(callICs[i].funGuard);
        jitCallICs[i].funJump = fullCode.locationOf(callICs[i].funJump);
        jitCallICs[i].slowPathStart = stubCode.locationOf(callICs[i].slowPathStart);
        jitCallICs[i].typeMonitored = callICs[i].typeMonitored;

        /* Compute the hot call offset. */
        uint32_t offset = fullCode.locationOf(callICs[i].hotJump) -
                        fullCode.locationOf(callICs[i].funGuard);
        jitCallICs[i].hotJumpOffset = offset;
        JS_ASSERT(jitCallICs[i].hotJumpOffset == offset);

        /* Compute the join point offset. */
        offset = fullCode.locationOf(callICs[i].joinPoint) -
                 fullCode.locationOf(callICs[i].funGuard);
        jitCallICs[i].joinPointOffset = offset;
        JS_ASSERT(jitCallICs[i].joinPointOffset == offset);

        /* Compute the OOL call offset. */
        offset = stubCode.locationOf(callICs[i].oolCall) -
                 stubCode.locationOf(callICs[i].slowPathStart);
        jitCallICs[i].oolCallOffset = offset;
        JS_ASSERT(jitCallICs[i].oolCallOffset == offset);

        /* Compute the OOL jump offset. */
        offset = stubCode.locationOf(callICs[i].oolJump) -
                 stubCode.locationOf(callICs[i].slowPathStart);
        jitCallICs[i].oolJumpOffset = offset;
        JS_ASSERT(jitCallICs[i].oolJumpOffset == offset);

        /* Compute the start of the OOL IC call. */
        offset = stubCode.locationOf(callICs[i].icCall) -
                 stubCode.locationOf(callICs[i].slowPathStart);
        jitCallICs[i].icCallOffset = offset;
        JS_ASSERT(jitCallICs[i].icCallOffset == offset);

        /* Compute the slow join point offset. */
        offset = stubCode.locationOf(callICs[i].slowJoinPoint) -
                 stubCode.locationOf(callICs[i].slowPathStart);
        jitCallICs[i].slowJoinOffset = offset;
        JS_ASSERT(jitCallICs[i].slowJoinOffset == offset);

        /* Compute the join point offset for continuing on the hot path. */
        offset = stubCode.locationOf(callICs[i].hotPathLabel) -
                 stubCode.locationOf(callICs[i].funGuard);
        jitCallICs[i].hotPathOffset = offset;
        JS_ASSERT(jitCallICs[i].hotPathOffset == offset);

        jitCallICs[i].call = &jitCallSites[callICs[i].callIndex];
        jitCallICs[i].frameSize = callICs[i].frameSize;
        jitCallICs[i].funObjReg = callICs[i].funObjReg;
        stubCode.patch(callICs[i].addrLabel1, &jitCallICs[i]);
        stubCode.patch(callICs[i].addrLabel2, &jitCallICs[i]);
    }

    ic::EqualityICInfo *jitEqualityICs = (ic::EqualityICInfo *)cursor;
    chunk->nEqualityICs = equalityICs.length();
    cursor += sizeof(ic::EqualityICInfo) * chunk->nEqualityICs;
    for (size_t i = 0; i < chunk->nEqualityICs; i++) {
        if (equalityICs[i].trampoline) {
            jitEqualityICs[i].target = stubCode.locationOf(equalityICs[i].trampolineStart);
        } else {
            uint32_t offs = uint32_t(equalityICs[i].jumpTarget - script->code);
            JS_ASSERT(jumpMap[offs].isSet());
            jitEqualityICs[i].target = fullCode.locationOf(jumpMap[offs]);
        }
        jitEqualityICs[i].stubEntry = stubCode.locationOf(equalityICs[i].stubEntry);
        jitEqualityICs[i].stubCall = stubCode.locationOf(equalityICs[i].stubCall);
        jitEqualityICs[i].stub = equalityICs[i].stub;
        jitEqualityICs[i].lvr = equalityICs[i].lvr;
        jitEqualityICs[i].rvr = equalityICs[i].rvr;
        jitEqualityICs[i].tempReg = equalityICs[i].tempReg;
        jitEqualityICs[i].cond = equalityICs[i].cond;
        if (equalityICs[i].jumpToStub.isSet())
            jitEqualityICs[i].jumpToStub = fullCode.locationOf(equalityICs[i].jumpToStub.get());
        jitEqualityICs[i].fallThrough = fullCode.locationOf(equalityICs[i].fallThrough);

        stubCode.patch(equalityICs[i].addrLabel, &jitEqualityICs[i]);
    }
#endif /* JS_MONOIC */

    for (size_t i = 0; i < callPatches.length(); i++) {
        CallPatchInfo &patch = callPatches[i];

        CodeLocationLabel joinPoint = patch.joinSlow
            ? stubCode.locationOf(patch.joinPoint)
            : fullCode.locationOf(patch.joinPoint);

        if (patch.hasFastNcode)
            fullCode.patch(patch.fastNcodePatch, joinPoint);
        if (patch.hasSlowNcode)
            stubCode.patch(patch.slowNcodePatch, joinPoint);
    }

#ifdef JS_POLYIC
    ic::GetElementIC *jitGetElems = (ic::GetElementIC *)cursor;
    chunk->nGetElems = getElemICs.length();
    cursor += sizeof(ic::GetElementIC) * chunk->nGetElems;
    for (size_t i = 0; i < chunk->nGetElems; i++) {
        ic::GetElementIC &to = jitGetElems[i];
        GetElementICInfo &from = getElemICs[i];

        new (&to) ic::GetElementIC();
        from.copyTo(to, fullCode, stubCode);

        to.typeReg = from.typeReg;
        to.objReg = from.objReg;
        to.idRemat = from.id;

        if (from.typeGuard.isSet()) {
            int inlineTypeGuard = fullCode.locationOf(from.typeGuard.get()) -
                                  fullCode.locationOf(from.fastPathStart);
            to.inlineTypeGuard = inlineTypeGuard;
            JS_ASSERT(to.inlineTypeGuard == inlineTypeGuard);
        }
        int inlineShapeGuard = fullCode.locationOf(from.shapeGuard) -
                               fullCode.locationOf(from.fastPathStart);
        to.inlineShapeGuard = inlineShapeGuard;
        JS_ASSERT(to.inlineShapeGuard == inlineShapeGuard);

        stubCode.patch(from.paramAddr, &to);
    }

    ic::SetElementIC *jitSetElems = (ic::SetElementIC *)cursor;
    chunk->nSetElems = setElemICs.length();
    cursor += sizeof(ic::SetElementIC) * chunk->nSetElems;
    for (size_t i = 0; i < chunk->nSetElems; i++) {
        ic::SetElementIC &to = jitSetElems[i];
        SetElementICInfo &from = setElemICs[i];

        new (&to) ic::SetElementIC();
        from.copyTo(to, fullCode, stubCode);

        to.strictMode = script->strictModeCode;
        to.vr = from.vr;
        to.objReg = from.objReg;
        to.objRemat = from.objRemat.toInt32();
        JS_ASSERT(to.objRemat == from.objRemat.toInt32());

        to.hasConstantKey = from.key.isConstant();
        if (from.key.isConstant())
            to.keyValue = from.key.index();
        else
            to.keyReg = from.key.reg();

        int inlineShapeGuard = fullCode.locationOf(from.shapeGuard) -
                               fullCode.locationOf(from.fastPathStart);
        to.inlineShapeGuard = inlineShapeGuard;
        JS_ASSERT(to.inlineShapeGuard == inlineShapeGuard);

        int inlineHoleGuard = fullCode.locationOf(from.holeGuard) -
                               fullCode.locationOf(from.fastPathStart);
        to.inlineHoleGuard = inlineHoleGuard;
        JS_ASSERT(to.inlineHoleGuard == inlineHoleGuard);

        CheckIsStubCall(to.slowPathCall.labelAtOffset(0));

        to.volatileMask = from.volatileMask;
        JS_ASSERT(to.volatileMask == from.volatileMask);

        stubCode.patch(from.paramAddr, &to);
    }

    ic::PICInfo *jitPics = (ic::PICInfo *)cursor;
    chunk->nPICs = pics.length();
    cursor += sizeof(ic::PICInfo) * chunk->nPICs;
    for (size_t i = 0; i < chunk->nPICs; i++) {
        new (&jitPics[i]) ic::PICInfo();
        pics[i].copyTo(jitPics[i], fullCode, stubCode);
        pics[i].copySimpleMembersTo(jitPics[i]);

        jitPics[i].shapeGuard = masm.distanceOf(pics[i].shapeGuard) -
                                masm.distanceOf(pics[i].fastPathStart);
        JS_ASSERT(jitPics[i].shapeGuard == masm.distanceOf(pics[i].shapeGuard) -
                                           masm.distanceOf(pics[i].fastPathStart));
        jitPics[i].shapeRegHasBaseShape = true;
        jitPics[i].pc = pics[i].pc;

        if (pics[i].kind == ic::PICInfo::SET) {
            jitPics[i].u.vr = pics[i].vr;
        } else if (pics[i].kind != ic::PICInfo::NAME) {
            if (pics[i].hasTypeCheck) {
                int32_t distance = stubcc.masm.distanceOf(pics[i].typeCheck) -
                                 stubcc.masm.distanceOf(pics[i].slowPathStart);
                JS_ASSERT(distance <= 0);
                jitPics[i].u.get.typeCheckOffset = distance;
            }
        }
        stubCode.patch(pics[i].paramAddr, &jitPics[i]);
    }
#endif

    JS_ASSERT(size_t(cursor - (uint8_t*)chunk) == dataSize);
    /* Use the computed size here -- we don't want slop bytes to be counted. */
    JS_ASSERT(chunk->computedSizeOfIncludingThis() == dataSize);

    /* Link fast and slow paths together. */
    stubcc.fixCrossJumps(result, masm.size(), masm.size() + stubcc.size());

#if defined(JS_CPU_MIPS)
    /* Make sure doubleOffset is aligned to sizeof(double) bytes.  */
    size_t doubleOffset = (((size_t)result + masm.size() + stubcc.size() +
                            sizeof(double) - 1) & (~(sizeof(double) - 1))) -
                          (size_t)result;
    JS_ASSERT((((size_t)result + doubleOffset) & 7) == 0);
#else
    size_t doubleOffset = masm.size() + stubcc.size();
#endif

    double *inlineDoubles = (double *) (result + doubleOffset);
    double *oolDoubles = (double*) (result + doubleOffset +
                                    masm.numDoubles() * sizeof(double));

    /* Generate jump tables. */
    void **jumpVec = (void **)(oolDoubles + stubcc.masm.numDoubles());

    for (size_t i = 0; i < jumpTableEdges.length(); i++) {
        JumpTableEdge edge = jumpTableEdges[i];
        if (bytecodeInChunk(script->code + edge.target)) {
            JS_ASSERT(jumpMap[edge.target].isSet());
            jumpVec[i] = (void *)(result + masm.distanceOf(jumpMap[edge.target]));
        } else {
            ChunkJumpTableEdge nedge;
            nedge.edge = edge;
            nedge.jumpTableEntry = &jumpVec[i];
            chunkJumps.infallibleAppend(nedge);
            jumpVec[i] = NULL;
        }
    }

    /* Patch jump table references. */
    for (size_t i = 0; i < jumpTables.length(); i++) {
        JumpTable &jumpTable = jumpTables[i];
        fullCode.patch(jumpTable.label, &jumpVec[jumpTable.offsetIndex]);
    }

    /* Patch all outgoing calls. */
    masm.finalize(fullCode, inlineDoubles);
    stubcc.masm.finalize(stubCode, oolDoubles);

    JSC::ExecutableAllocator::makeExecutable(result, masm.size() + stubcc.size());
    JSC::ExecutableAllocator::cacheFlush(result, masm.size() + stubcc.size());

    Probes::registerMJITCode(cx, chunk,
                             a,
                             (JSActiveFrame**) inlineFrames.begin(),
                             result, masm.size(),
                             result + masm.size(), stubcc.size());

    outerChunkRef().chunk = chunk;

    /* Patch all incoming and outgoing cross-chunk jumps. */
    CrossChunkEdge *crossEdges = jit->edges();
    for (unsigned i = 0; i < jit->nedges; i++) {
        CrossChunkEdge &edge = crossEdges[i];
        if (bytecodeInChunk(outerScript->code + edge.source)) {
            JS_ASSERT(!edge.sourceJump1 && !edge.sourceJump2);
            void *label = edge.targetLabel ? edge.targetLabel : edge.shimLabel;
            CodeLocationLabel targetLabel(label);
            JSOp op = JSOp(script->code[edge.source]);
            if (op == JSOP_TABLESWITCH) {
                if (edge.jumpTableEntries)
                    cx->free_(edge.jumpTableEntries);
                CrossChunkEdge::JumpTableEntryVector *jumpTableEntries = NULL;
                bool failed = false;
                for (unsigned j = 0; j < chunkJumps.length(); j++) {
                    ChunkJumpTableEdge nedge = chunkJumps[j];
                    if (nedge.edge.source == edge.source && nedge.edge.target == edge.target) {
                        if (!jumpTableEntries) {
                            jumpTableEntries = OffTheBooks::new_<CrossChunkEdge::JumpTableEntryVector>();
                            if (!jumpTableEntries)
                                failed = true;
                        }
                        if (!jumpTableEntries->append(nedge.jumpTableEntry))
                            failed = true;
                        *nedge.jumpTableEntry = label;
                    }
                }
                if (failed) {
                    execPool->release();
                    cx->free_(chunk);
                    js_ReportOutOfMemory(cx);
                    return Compile_Error;
                }
                edge.jumpTableEntries = jumpTableEntries;
            }
            for (unsigned j = 0; j < chunkEdges.length(); j++) {
                const OutgoingChunkEdge &oedge = chunkEdges[j];
                if (oedge.source == edge.source && oedge.target == edge.target) {
                    /*
                     * Only a single edge needs to be patched; we ensured while
                     * generating chunks that no two cross chunk edges can have
                     * the same source and target. Note that there may not be
                     * an edge to patch, if constant folding determined the
                     * jump is never taken.
                     */
                    edge.sourceJump1 = fullCode.locationOf(oedge.fastJump).executableAddress();
                    if (oedge.slowJump.isSet()) {
                        edge.sourceJump2 =
                            stubCode.locationOf(oedge.slowJump.get()).executableAddress();
                    }
#ifdef JS_CPU_X64
                    edge.sourceTrampoline =
                        stubCode.locationOf(oedge.sourceTrampoline).executableAddress();
#endif
                    jit->patchEdge(edge, label);
                    break;
                }
            }
        } else if (bytecodeInChunk(outerScript->code + edge.target)) {
            JS_ASSERT(!edge.targetLabel);
            JS_ASSERT(jumpMap[edge.target].isSet());
            edge.targetLabel = fullCode.locationOf(jumpMap[edge.target]).executableAddress();
            jit->patchEdge(edge, edge.targetLabel);
        }
    }

    return Compile_Okay;
}

#ifdef DEBUG
#define SPEW_OPCODE()                                                         \
    JS_BEGIN_MACRO                                                            \
        if (IsJaegerSpewChannelActive(JSpew_JSOps)) {                         \
            Sprinter sprinter(cx);                                            \
            sprinter.init();                                                  \
            js_Disassemble1(cx, script, PC, PC - script->code,                \
                            JS_TRUE, &sprinter);                              \
            JaegerSpew(JSpew_JSOps, "    %2d %s",                             \
                       frame.stackDepth(), sprinter.string());                \
        }                                                                     \
    JS_END_MACRO;
#else
#define SPEW_OPCODE()
#endif /* DEBUG */

#define BEGIN_CASE(name)        case name:
#define END_CASE(name)                      \
    JS_BEGIN_MACRO                          \
        PC += name##_LENGTH;                \
    JS_END_MACRO;                           \
    break;

static inline void
FixDouble(Value &val)
{
    if (val.isInt32())
        val.setDouble((double)val.toInt32());
}

inline bool
mjit::Compiler::shouldStartLoop(jsbytecode *head)
{
    /*
     * Don't do loop based optimizations or register allocation for loops which
     * span multiple chunks.
     */
    if (*head == JSOP_LOOPHEAD && analysis->getLoop(head)) {
        uint32_t backedge = analysis->getLoop(head)->backedge;
        if (!bytecodeInChunk(script->code + backedge))
            return false;
        return true;
    }
    return false;
}

CompileStatus
mjit::Compiler::generateMethod()
{
    SrcNoteLineScanner scanner(script->notes(), script->lineno);

    /* For join points, whether there was fallthrough from the previous opcode. */
    bool fallthrough = true;

    /* Last bytecode processed. */
    jsbytecode *lastPC = NULL;

    if (!outerJIT())
        return Compile_Retry;

    uint32_t chunkBegin = 0, chunkEnd = script->length;
    if (!a->parent) {
        const ChunkDescriptor &desc =
            outerJIT()->chunkDescriptor(chunkIndex);
        chunkBegin = desc.begin;
        chunkEnd = desc.end;

        while (PC != script->code + chunkBegin) {
            Bytecode *opinfo = analysis->maybeCode(PC);
            if (opinfo) {
                if (opinfo->jumpTarget) {
                    /* Update variable types for all new values at this bytecode. */
                    const SlotValue *newv = analysis->newValues(PC);
                    if (newv) {
                        while (newv->slot) {
                            if (newv->slot < TotalSlots(script)) {
                                VarType &vt = a->varTypes[newv->slot];
                                vt.setTypes(analysis->getValueTypes(newv->value));
                            }
                            newv++;
                        }
                    }
                }
                if (analyze::BytecodeUpdatesSlot(JSOp(*PC))) {
                    uint32_t slot = GetBytecodeSlot(script, PC);
                    if (analysis->trackSlot(slot)) {
                        VarType &vt = a->varTypes[slot];
                        vt.setTypes(analysis->pushedTypes(PC, 0));
                    }
                }
            }

            PC += GetBytecodeLength(PC);
        }

        if (chunkIndex != 0) {
            uint32_t depth = analysis->getCode(PC).stackDepth;
            for (uint32_t i = 0; i < depth; i++)
                frame.pushSynced(JSVAL_TYPE_UNKNOWN);
        }
    }

    for (;;) {
        JSOp op = JSOp(*PC);
        int trap = stubs::JSTRAP_NONE;

        if (script->hasBreakpointsAt(PC))
            trap |= stubs::JSTRAP_TRAP;

        Bytecode *opinfo = analysis->maybeCode(PC);

        if (!opinfo) {
            if (op == JSOP_STOP)
                break;
            if (js_CodeSpec[op].length != -1)
                PC += js_CodeSpec[op].length;
            else
                PC += js_GetVariableBytecodeLength(PC);
            continue;
        }

        if (PC >= script->code + script->length)
            break;

        scanner.advanceTo(PC - script->code);
        if (script->stepModeEnabled() &&
            (scanner.isLineHeader() || opinfo->jumpTarget))
        {
            trap |= stubs::JSTRAP_SINGLESTEP;
        }

        frame.setPC(PC);
        frame.setInTryBlock(opinfo->inTryBlock);

        if (fallthrough) {
            /*
             * If there is fallthrough from the previous opcode and we changed
             * any entries into doubles for a branch at that previous op,
             * revert those entries into integers. Similarly, if we forgot that
             * an entry is a double then make it a double again, as the frame
             * may have assigned it a normal register.
             */
            for (unsigned i = 0; i < fixedIntToDoubleEntries.length(); i++) {
                FrameEntry *fe = frame.getSlotEntry(fixedIntToDoubleEntries[i]);
                frame.ensureInteger(fe);
            }
            for (unsigned i = 0; i < fixedDoubleToAnyEntries.length(); i++) {
                FrameEntry *fe = frame.getSlotEntry(fixedDoubleToAnyEntries[i]);
                frame.syncAndForgetFe(fe);
            }
        }
        fixedIntToDoubleEntries.clear();
        fixedDoubleToAnyEntries.clear();

        if (PC >= script->code + chunkEnd) {
            if (fallthrough) {
                if (opinfo->jumpTarget)
                    fixDoubleTypes(PC);
                frame.syncAndForgetEverything();
                jsbytecode *curPC = PC;
                do {
                    PC--;
                } while (!analysis->maybeCode(PC));
                if (!jumpAndRun(masm.jump(), curPC, NULL, NULL, /* fallthrough = */ true))
                    return Compile_Error;
                PC = curPC;
            }
            break;
        }

        if (opinfo->jumpTarget || trap) {
            if (fallthrough) {
                fixDoubleTypes(PC);
                fixedIntToDoubleEntries.clear();
                fixedDoubleToAnyEntries.clear();

                /*
                 * Watch for fallthrough to the head of a 'do while' loop.
                 * We don't know what register state we will be using at the head
                 * of the loop so sync, branch, and fix it up after the loop
                 * has been processed.
                 */
                if (cx->typeInferenceEnabled() && shouldStartLoop(PC)) {
                    frame.syncAndForgetEverything();
                    Jump j = masm.jump();
                    if (!startLoop(PC, j, PC))
                        return Compile_Error;
                } else {
                    Label start = masm.label();
                    if (!frame.syncForBranch(PC, Uses(0)))
                        return Compile_Error;
                    if (pcLengths && lastPC) {
                        /* Track this sync code for the previous op. */
                        size_t length = masm.size() - masm.distanceOf(start);
                        uint32_t offset = ssa.frameLength(a->inlineIndex) + lastPC - script->code;
                        pcLengths[offset].codeLength += length;
                    }
                    JS_ASSERT(frame.consistentRegisters(PC));
                }
            }

            if (!frame.discardForJoin(analysis->getAllocation(PC), opinfo->stackDepth))
                return Compile_Error;
            updateJoinVarTypes();
            fallthrough = true;

            if (!cx->typeInferenceEnabled()) {
                /* All join points have synced state if we aren't doing cross-branch regalloc. */
                opinfo->safePoint = true;
            }
        } else if (opinfo->safePoint) {
            frame.syncAndForgetEverything();
        }
        frame.assertValidRegisterState();
        a->jumpMap[uint32_t(PC - script->code)] = masm.label();

        if (cx->typeInferenceEnabled() && opinfo->safePoint) {
            /*
             * We may have come in from a table switch, which does not watch
             * for the new types introduced for variables at each dispatch
             * target. Make sure that new SSA values at this safe point with
             * double type have the correct in memory representation.
             */
            const SlotValue *newv = analysis->newValues(PC);
            if (newv) {
                while (newv->slot) {
                    if (newv->value.kind() == SSAValue::PHI &&
                        newv->value.phiOffset() == uint32_t(PC - script->code) &&
                        analysis->trackSlot(newv->slot) &&
                        a->varTypes[newv->slot].getTypeTag(cx) == JSVAL_TYPE_DOUBLE) {
                        FrameEntry *fe = frame.getSlotEntry(newv->slot);
                        masm.ensureInMemoryDouble(frame.addressOf(fe));
                    }
                    newv++;
                }
            }
        }

        // Now that we have the PC's register allocation, make sure it gets
        // explicitly updated if this is the loop entry and new loop registers
        // are allocated later on.
        if (loop && !a->parent)
            loop->setOuterPC(PC);

        SPEW_OPCODE();
        JS_ASSERT(frame.stackDepth() == opinfo->stackDepth);

        if (op == JSOP_LOOPHEAD && analysis->getLoop(PC)) {
            jsbytecode *backedge = script->code + analysis->getLoop(PC)->backedge;
            if (!bytecodeInChunk(backedge)){
                for (uint32_t slot = ArgSlot(0); slot < TotalSlots(script); slot++) {
                    if (a->varTypes[slot].getTypeTag(cx) == JSVAL_TYPE_DOUBLE) {
                        FrameEntry *fe = frame.getSlotEntry(slot);
                        masm.ensureInMemoryDouble(frame.addressOf(fe));
                    }
                }
            }
        }

        // If this is an exception entry point, then jsl_InternalThrow has set
        // VMFrame::fp to the correct fp for the entry point. We need to copy
        // that value here to FpReg so that FpReg also has the correct sp.
        // Otherwise, we would simply be using a stale FpReg value.
        if (op == JSOP_ENTERBLOCK && analysis->getCode(PC).exceptionEntry)
            masm.loadPtr(FrameAddress(VMFrame::offsetOfFp), JSFrameReg);

        if (trap) {
            prepareStubCall(Uses(0));
            masm.move(Imm32(trap), Registers::ArgReg1);
            Call cl = emitStubCall(JS_FUNC_TO_DATA_PTR(void *, stubs::Trap), NULL);
            InternalCallSite site(masm.callReturnOffset(cl), a->inlineIndex, PC,
                                  REJOIN_TRAP, false);
            addCallSite(site);
        }

        /* Don't compile fat opcodes, run the decomposed version instead. */
        if (js_CodeSpec[op].format & JOF_DECOMPOSE) {
            PC += js_CodeSpec[op].length;
            continue;
        }

        Label codeStart = masm.label();
        bool countsUpdated = false;
        bool arithUpdated = false;

        JSValueType arithFirstUseType = JSVAL_TYPE_UNKNOWN;
        JSValueType arithSecondUseType = JSVAL_TYPE_UNKNOWN;
        if (script->hasScriptCounts && !!(js_CodeSpec[op].format & JOF_ARITH)) {
            if (GetUseCount(script, PC - script->code) == 1) {
                FrameEntry *use = frame.peek(-1);
                /*
                 * Pretend it's a binary operation and the second operand has
                 * the same type as the first one.
                 */
                if (use->isTypeKnown())
                    arithFirstUseType = arithSecondUseType = use->getKnownType();
            } else {
                FrameEntry *use = frame.peek(-1);
                if (use->isTypeKnown())
                    arithFirstUseType = use->getKnownType();
                use = frame.peek(-2);
                if (use->isTypeKnown())
                    arithSecondUseType = use->getKnownType();
            }
        }

        /*
         * Update PC counts for jump opcodes at their start, so that we don't
         * miss them when taking the jump. This is delayed for other opcodes,
         * as we want to skip updating for ops we didn't generate any code for.
         */
        if (script->hasScriptCounts && JOF_OPTYPE(op) == JOF_JUMP)
            updatePCCounts(PC, &codeStart, &countsUpdated);

    /**********************
     * BEGIN COMPILER OPS *
     **********************/

        lastPC = PC;

        switch (op) {
          BEGIN_CASE(JSOP_NOP)
          END_CASE(JSOP_NOP)

          BEGIN_CASE(JSOP_NOTEARG)
          END_CASE(JSOP_NOTEARG)
          
          BEGIN_CASE(JSOP_UNDEFINED)
            frame.push(UndefinedValue());
          END_CASE(JSOP_UNDEFINED)

          BEGIN_CASE(JSOP_POPV)
          BEGIN_CASE(JSOP_SETRVAL)
          {
            RegisterID reg = frame.allocReg();
            masm.load32(FrameFlagsAddress(), reg);
            masm.or32(Imm32(StackFrame::HAS_RVAL), reg);
            masm.store32(reg, FrameFlagsAddress());
            frame.freeReg(reg);

            /* Scripts which write to the frame's return slot aren't inlined. */
            JS_ASSERT(a == outer);

            FrameEntry *fe = frame.peek(-1);
            frame.storeTo(fe, Address(JSFrameReg, StackFrame::offsetOfReturnValue()), true);
            frame.pop();
          }
          END_CASE(JSOP_POPV)

          BEGIN_CASE(JSOP_RETURN)
            if (script->hasScriptCounts)
                updatePCCounts(PC, &codeStart, &countsUpdated);
            emitReturn(frame.peek(-1));
            fallthrough = false;
          END_CASE(JSOP_RETURN)

          BEGIN_CASE(JSOP_GOTO)
          BEGIN_CASE(JSOP_DEFAULT)
          {
            unsigned targetOffset = FollowBranch(cx, script, PC - script->code);
            jsbytecode *target = script->code + targetOffset;

            fixDoubleTypes(target);

            /*
             * Watch for gotos which are entering a 'for' or 'while' loop.
             * These jump to the loop condition test and are immediately
             * followed by the head of the loop.
             */
            jsbytecode *next = PC + js_CodeSpec[op].length;
            if (cx->typeInferenceEnabled() &&
                analysis->maybeCode(next) &&
                shouldStartLoop(next))
            {
                frame.syncAndForgetEverything();
                Jump j = masm.jump();
                if (!startLoop(next, j, target))
                    return Compile_Error;
            } else {
                if (!frame.syncForBranch(target, Uses(0)))
                    return Compile_Error;
                Jump j = masm.jump();
                if (!jumpAndRun(j, target))
                    return Compile_Error;
            }
            fallthrough = false;
            PC += js_CodeSpec[op].length;
            break;
          }
          END_CASE(JSOP_GOTO)

          BEGIN_CASE(JSOP_IFEQ)
          BEGIN_CASE(JSOP_IFNE)
          {
            jsbytecode *target = PC + GET_JUMP_OFFSET(PC);
            fixDoubleTypes(target);
            if (!jsop_ifneq(op, target))
                return Compile_Error;
            PC += js_CodeSpec[op].length;
            break;
          }
          END_CASE(JSOP_IFNE)

          BEGIN_CASE(JSOP_ARGUMENTS)
            if (script->needsArgsObj()) {
                prepareStubCall(Uses(0));
                INLINE_STUBCALL(stubs::Arguments, REJOIN_FALLTHROUGH);
                pushSyncedEntry(0);
            } else {
                frame.push(MagicValue(JS_OPTIMIZED_ARGUMENTS));
            }
          END_CASE(JSOP_ARGUMENTS)
          BEGIN_CASE(JSOP_ACTUALSFILLED)
          {

            // We never inline things with defaults because of the switch.
            JS_ASSERT(a == outer);
            RegisterID value = frame.allocReg(), nactual = frame.allocReg();
            int32_t defstart = GET_UINT16(PC);
            masm.move(Imm32(defstart), value);
            masm.load32(Address(JSFrameReg, StackFrame::offsetOfNumActual()), nactual);

            // Best would be a single instruction where available (like
            // cmovge on x86), but there's no way get that yet, so jump.
            Jump j = masm.branch32(Assembler::LessThan, nactual, Imm32(defstart));
            masm.move(nactual, value);
            j.linkTo(masm.label(), &masm);
            frame.freeReg(nactual);
            frame.pushInt32(value);
          }
          END_CASE(JSOP_ACTUALSFILLED)

          BEGIN_CASE(JSOP_ITERNEXT)
            iterNext(GET_INT8(PC));
          END_CASE(JSOP_ITERNEXT)

          BEGIN_CASE(JSOP_DUP)
            frame.dup();
          END_CASE(JSOP_DUP)

          BEGIN_CASE(JSOP_DUP2)
            frame.dup2();
          END_CASE(JSOP_DUP2)

          BEGIN_CASE(JSOP_SWAP)
            frame.dup2();
            frame.shift(-3);
            frame.shift(-1);
          END_CASE(JSOP_SWAP)

          BEGIN_CASE(JSOP_PICK)
          {
            uint32_t amt = GET_UINT8(PC);

            // Push -(amt + 1), say amt == 2
            // Stack before: X3 X2 X1
            // Stack after:  X3 X2 X1 X3
            frame.dupAt(-int32_t(amt + 1));

            // For each item X[i...1] push it then move it down.
            // The above would transition like so:
            //   X3 X2 X1 X3 X2 (dupAt)
            //   X2 X2 X1 X3    (shift)
            //   X2 X2 X1 X3 X1 (dupAt)
            //   X2 X1 X1 X3    (shift)
            for (int32_t i = -int32_t(amt); i < 0; i++) {
                frame.dupAt(i - 1);
                frame.shift(i - 2);
            }

            // The stack looks like:
            // Xn ... X1 X1 X{n+1}
            // So shimmy the last value down.
            frame.shimmy(1);
          }
          END_CASE(JSOP_PICK)

          BEGIN_CASE(JSOP_BITOR)
          BEGIN_CASE(JSOP_BITXOR)
          BEGIN_CASE(JSOP_BITAND)
            jsop_bitop(op);
          END_CASE(JSOP_BITAND)

          BEGIN_CASE(JSOP_LT)
          BEGIN_CASE(JSOP_LE)
          BEGIN_CASE(JSOP_GT)
          BEGIN_CASE(JSOP_GE)
          BEGIN_CASE(JSOP_EQ)
          BEGIN_CASE(JSOP_NE)
          {
           if (script->hasScriptCounts) {
               updateArithCounts(PC, NULL, arithFirstUseType, arithSecondUseType);
               arithUpdated = true;
           }

            /* Detect fusions. */
            jsbytecode *next = &PC[JSOP_GE_LENGTH];
            JSOp fused = JSOp(*next);
            if ((fused != JSOP_IFEQ && fused != JSOP_IFNE) || analysis->jumpTarget(next))
                fused = JSOP_NOP;

            /* Get jump target, if any. */
            jsbytecode *target = NULL;
            if (fused != JSOP_NOP) {
                if (script->hasScriptCounts)
                    updatePCCounts(PC, &codeStart, &countsUpdated);
                target = next + GET_JUMP_OFFSET(next);
                fixDoubleTypes(target);
            }

            BoolStub stub = NULL;
            switch (op) {
              case JSOP_LT:
                stub = stubs::LessThan;
                break;
              case JSOP_LE:
                stub = stubs::LessEqual;
                break;
              case JSOP_GT:
                stub = stubs::GreaterThan;
                break;
              case JSOP_GE:
                stub = stubs::GreaterEqual;
                break;
              case JSOP_EQ:
                stub = stubs::Equal;
                break;
              case JSOP_NE:
                stub = stubs::NotEqual;
                break;
              default:
                JS_NOT_REACHED("WAT");
                break;
            }

            /*
             * We need to ensure in the target case that we always rejoin
             * before the rval test. In the non-target case we will rejoin
             * correctly after the op finishes.
             */

            FrameEntry *rhs = frame.peek(-1);
            FrameEntry *lhs = frame.peek(-2);

            /* Check for easy cases that the parser does not constant fold. */
            if (lhs->isConstant() && rhs->isConstant()) {
                /* Primitives can be trivially constant folded. */
                const Value &lv = lhs->getValue();
                const Value &rv = rhs->getValue();

                if (lv.isPrimitive() && rv.isPrimitive()) {
                    bool result = compareTwoValues(cx, op, lv, rv);

                    frame.pop();
                    frame.pop();

                    if (!target) {
                        frame.push(Value(BooleanValue(result)));
                    } else {
                        if (fused == JSOP_IFEQ)
                            result = !result;
                        if (!constantFoldBranch(target, result))
                            return Compile_Error;
                    }
                } else {
                    if (!emitStubCmpOp(stub, target, fused))
                        return Compile_Error;
                }
            } else {
                /* Anything else should go through the fast path generator. */
                if (!jsop_relational(op, stub, target, fused))
                    return Compile_Error;
            }

            /* Advance PC manually. */
            JS_STATIC_ASSERT(JSOP_LT_LENGTH == JSOP_GE_LENGTH);
            JS_STATIC_ASSERT(JSOP_LE_LENGTH == JSOP_GE_LENGTH);
            JS_STATIC_ASSERT(JSOP_GT_LENGTH == JSOP_GE_LENGTH);
            JS_STATIC_ASSERT(JSOP_EQ_LENGTH == JSOP_GE_LENGTH);
            JS_STATIC_ASSERT(JSOP_NE_LENGTH == JSOP_GE_LENGTH);

            PC += JSOP_GE_LENGTH;
            if (fused != JSOP_NOP) {
                SPEW_OPCODE();
                PC += JSOP_IFNE_LENGTH;
            }
            break;
          }
          END_CASE(JSOP_GE)

          BEGIN_CASE(JSOP_LSH)
            jsop_bitop(op);
          END_CASE(JSOP_LSH)

          BEGIN_CASE(JSOP_RSH)
            jsop_bitop(op);
          END_CASE(JSOP_RSH)

          BEGIN_CASE(JSOP_URSH)
            jsop_bitop(op);
          END_CASE(JSOP_URSH)

          BEGIN_CASE(JSOP_ADD)
            if (!jsop_binary(op, stubs::Add, knownPushedType(0), pushedTypeSet(0)))
                return Compile_Retry;
          END_CASE(JSOP_ADD)

          BEGIN_CASE(JSOP_SUB)
            if (!jsop_binary(op, stubs::Sub, knownPushedType(0), pushedTypeSet(0)))
                return Compile_Retry;
          END_CASE(JSOP_SUB)

          BEGIN_CASE(JSOP_MUL)
            if (!jsop_binary(op, stubs::Mul, knownPushedType(0), pushedTypeSet(0)))
                return Compile_Retry;
          END_CASE(JSOP_MUL)

          BEGIN_CASE(JSOP_DIV)
            if (!jsop_binary(op, stubs::Div, knownPushedType(0), pushedTypeSet(0)))
                return Compile_Retry;
          END_CASE(JSOP_DIV)

          BEGIN_CASE(JSOP_MOD)
            if (!jsop_mod())
                return Compile_Retry;
          END_CASE(JSOP_MOD)

          BEGIN_CASE(JSOP_NOT)
            jsop_not();
          END_CASE(JSOP_NOT)

          BEGIN_CASE(JSOP_BITNOT)
          {
            FrameEntry *top = frame.peek(-1);
            if (top->isConstant() && top->getValue().isPrimitive()) {
                int32_t i;
                JS_ALWAYS_TRUE(ToInt32(cx, top->getValue(), &i));
                i = ~i;
                frame.pop();
                frame.push(Int32Value(i));
            } else {
                jsop_bitnot();
            }
          }
          END_CASE(JSOP_BITNOT)

          BEGIN_CASE(JSOP_NEG)
          {
            FrameEntry *top = frame.peek(-1);
            if (top->isConstant() && top->getValue().isPrimitive()) {
                double d;
                JS_ALWAYS_TRUE(ToNumber(cx, top->getValue(), &d));
                d = -d;
                Value v = NumberValue(d);

                /* Watch for overflow in constant propagation. */
                types::TypeSet *pushed = pushedTypeSet(0);
                if (!v.isInt32() && pushed && !pushed->hasType(types::Type::DoubleType())) {
                    types::TypeScript::MonitorOverflow(cx, script, PC);
                    return Compile_Retry;
                }

                frame.pop();
                frame.push(v);
            } else {
                jsop_neg();
            }
          }
          END_CASE(JSOP_NEG)

          BEGIN_CASE(JSOP_POS)
            jsop_pos();
          END_CASE(JSOP_POS)

          BEGIN_CASE(JSOP_DELNAME)
          {
            uint32_t index = GET_UINT32_INDEX(PC);
            PropertyName *name = script->getName(index);

            prepareStubCall(Uses(0));
            masm.move(ImmPtr(name), Registers::ArgReg1);
            INLINE_STUBCALL(stubs::DelName, REJOIN_FALLTHROUGH);
            pushSyncedEntry(0);
          }
          END_CASE(JSOP_DELNAME)

          BEGIN_CASE(JSOP_DELPROP)
          {
            uint32_t index = GET_UINT32_INDEX(PC);
            PropertyName *name = script->getName(index);

            prepareStubCall(Uses(1));
            masm.move(ImmPtr(name), Registers::ArgReg1);
            INLINE_STUBCALL(STRICT_VARIANT(stubs::DelProp), REJOIN_FALLTHROUGH);
            frame.pop();
            pushSyncedEntry(0);
          }
          END_CASE(JSOP_DELPROP)

          BEGIN_CASE(JSOP_DELELEM)
          {
            prepareStubCall(Uses(2));
            INLINE_STUBCALL(STRICT_VARIANT(stubs::DelElem), REJOIN_FALLTHROUGH);
            frame.popn(2);
            pushSyncedEntry(0);
          }
          END_CASE(JSOP_DELELEM)

          BEGIN_CASE(JSOP_TYPEOF)
          BEGIN_CASE(JSOP_TYPEOFEXPR)
            jsop_typeof();
          END_CASE(JSOP_TYPEOF)

          BEGIN_CASE(JSOP_VOID)
            frame.pop();
            frame.push(UndefinedValue());
          END_CASE(JSOP_VOID)

          BEGIN_CASE(JSOP_GETPROP)
          BEGIN_CASE(JSOP_CALLPROP)
          BEGIN_CASE(JSOP_LENGTH)
            if (!jsop_getprop(script->getName(GET_UINT32_INDEX(PC)), knownPushedType(0)))
                return Compile_Error;
          END_CASE(JSOP_GETPROP)

          BEGIN_CASE(JSOP_GETELEM)
          BEGIN_CASE(JSOP_CALLELEM)
            if (script->hasScriptCounts)
                updateElemCounts(PC, frame.peek(-2), frame.peek(-1));
            if (!jsop_getelem())
                return Compile_Error;
          END_CASE(JSOP_GETELEM)

          BEGIN_CASE(JSOP_TOID)
            jsop_toid();
          END_CASE(JSOP_TOID)

          BEGIN_CASE(JSOP_SETELEM)
          {
            if (script->hasScriptCounts)
                updateElemCounts(PC, frame.peek(-3), frame.peek(-2));
            jsbytecode *next = &PC[JSOP_SETELEM_LENGTH];
            bool pop = (JSOp(*next) == JSOP_POP && !analysis->jumpTarget(next));
            if (!jsop_setelem(pop))
                return Compile_Error;
          }
          END_CASE(JSOP_SETELEM);

          BEGIN_CASE(JSOP_EVAL)
          {
            JaegerSpew(JSpew_Insns, " --- EVAL --- \n");
            emitEval(GET_ARGC(PC));
            JaegerSpew(JSpew_Insns, " --- END EVAL --- \n");
          }
          END_CASE(JSOP_EVAL)

          BEGIN_CASE(JSOP_CALL)
          BEGIN_CASE(JSOP_NEW)
          BEGIN_CASE(JSOP_FUNAPPLY)
          BEGIN_CASE(JSOP_FUNCALL)
          {
            bool callingNew = (op == JSOP_NEW);

            bool done = false;
            if ((op == JSOP_CALL || op == JSOP_NEW) && !monitored(PC)) {
                CompileStatus status = inlineNativeFunction(GET_ARGC(PC), callingNew);
                if (status == Compile_Okay)
                    done = true;
                else if (status != Compile_InlineAbort)
                    return status;
            }
            if (!done && inlining()) {
                CompileStatus status = inlineScriptedFunction(GET_ARGC(PC), callingNew);
                if (status == Compile_Okay)
                    done = true;
                else if (status != Compile_InlineAbort)
                    return status;
                if (script->hasScriptCounts) {
                    /* Code generated while inlining has been accounted for. */
                    updatePCCounts(PC, &codeStart, &countsUpdated);
                }
            }

            FrameSize frameSize;
            frameSize.initStatic(frame.totalDepth(), GET_ARGC(PC));

            if (!done) {
                JaegerSpew(JSpew_Insns, " --- SCRIPTED CALL --- \n");
                if (!inlineCallHelper(GET_ARGC(PC), callingNew, frameSize))
                    return Compile_Error;
                JaegerSpew(JSpew_Insns, " --- END SCRIPTED CALL --- \n");
            }
          }
          END_CASE(JSOP_CALL)

          BEGIN_CASE(JSOP_NAME)
          BEGIN_CASE(JSOP_CALLNAME)
          {
            PropertyName *name = script->getName(GET_UINT32_INDEX(PC));
            jsop_name(name, knownPushedType(0));
            frame.extra(frame.peek(-1)).name = name;
          }
          END_CASE(JSOP_NAME)

          BEGIN_CASE(JSOP_IMPLICITTHIS)
          {
            prepareStubCall(Uses(0));
            masm.move(ImmPtr(script->getName(GET_UINT32_INDEX(PC))), Registers::ArgReg1);
            INLINE_STUBCALL(stubs::ImplicitThis, REJOIN_FALLTHROUGH);
            frame.pushSynced(JSVAL_TYPE_UNKNOWN);
          }
          END_CASE(JSOP_IMPLICITTHIS)

          BEGIN_CASE(JSOP_DOUBLE)
          {
            double d = script->getConst(GET_UINT32_INDEX(PC)).toDouble();
            frame.push(Value(DoubleValue(d)));
          }
          END_CASE(JSOP_DOUBLE)

          BEGIN_CASE(JSOP_STRING)
            frame.push(StringValue(script->getAtom(GET_UINT32_INDEX(PC))));
          END_CASE(JSOP_STRING)

          BEGIN_CASE(JSOP_ZERO)
            frame.push(JSVAL_ZERO);
          END_CASE(JSOP_ZERO)

          BEGIN_CASE(JSOP_ONE)
            frame.push(JSVAL_ONE);
          END_CASE(JSOP_ONE)

          BEGIN_CASE(JSOP_NULL)
            frame.push(NullValue());
          END_CASE(JSOP_NULL)

          BEGIN_CASE(JSOP_THIS)
            jsop_this();
          END_CASE(JSOP_THIS)

          BEGIN_CASE(JSOP_FALSE)
            frame.push(Value(BooleanValue(false)));
          END_CASE(JSOP_FALSE)

          BEGIN_CASE(JSOP_TRUE)
            frame.push(Value(BooleanValue(true)));
          END_CASE(JSOP_TRUE)

          BEGIN_CASE(JSOP_OR)
          BEGIN_CASE(JSOP_AND)
          {
            jsbytecode *target = PC + GET_JUMP_OFFSET(PC);
            fixDoubleTypes(target);
            if (!jsop_andor(op, target))
                return Compile_Error;
          }
          END_CASE(JSOP_AND)

          BEGIN_CASE(JSOP_TABLESWITCH)
            /*
             * Note: there is no need to syncForBranch for the various targets of
             * switch statement. The liveness analysis has already marked these as
             * allocated with no registers in use. There is also no need to fix
             * double types, as we don't track types of slots in scripts with
             * switch statements (could be fixed).
             */
            if (script->hasScriptCounts)
                updatePCCounts(PC, &codeStart, &countsUpdated);
#if defined JS_CPU_ARM /* Need to implement jump(BaseIndex) for ARM */
            frame.syncAndKillEverything();
            masm.move(ImmPtr(PC), Registers::ArgReg1);

            /* prepareStubCall() is not needed due to syncAndForgetEverything() */
            INLINE_STUBCALL(stubs::TableSwitch, REJOIN_NONE);
            frame.pop();

            masm.jump(Registers::ReturnReg);
#else
            if (!jsop_tableswitch(PC))
                return Compile_Error;
#endif
            PC += js_GetVariableBytecodeLength(PC);
            break;
          END_CASE(JSOP_TABLESWITCH)

          BEGIN_CASE(JSOP_LOOKUPSWITCH)
            if (script->hasScriptCounts)
                updatePCCounts(PC, &codeStart, &countsUpdated);
            frame.syncAndForgetEverything();
            masm.move(ImmPtr(PC), Registers::ArgReg1);

            /* prepareStubCall() is not needed due to syncAndForgetEverything() */
            INLINE_STUBCALL(stubs::LookupSwitch, REJOIN_NONE);
            frame.pop();

            masm.jump(Registers::ReturnReg);
            PC += js_GetVariableBytecodeLength(PC);
            break;
          END_CASE(JSOP_LOOKUPSWITCH)

          BEGIN_CASE(JSOP_CASE)
            // X Y

            frame.dupAt(-2);
            // X Y X

            jsop_stricteq(JSOP_STRICTEQ);
            // X cond

            if (!jsop_ifneq(JSOP_IFNE, PC + GET_JUMP_OFFSET(PC)))
                return Compile_Error;
          END_CASE(JSOP_CASE)

          BEGIN_CASE(JSOP_STRICTEQ)
          BEGIN_CASE(JSOP_STRICTNE)
            if (script->hasScriptCounts) {
                updateArithCounts(PC, NULL, arithFirstUseType, arithSecondUseType);
                arithUpdated = true;
            }
            jsop_stricteq(op);
          END_CASE(JSOP_STRICTEQ)

          BEGIN_CASE(JSOP_ITER)
            if (!iter(GET_UINT8(PC)))
                return Compile_Error;
          END_CASE(JSOP_ITER)

          BEGIN_CASE(JSOP_MOREITER)
          {
            /* At the byte level, this is always fused with IFNE or IFNEX. */
            if (script->hasScriptCounts)
                updatePCCounts(PC, &codeStart, &countsUpdated);
            jsbytecode *target = &PC[JSOP_MOREITER_LENGTH];
            JSOp next = JSOp(*target);
            JS_ASSERT(next == JSOP_IFNE);

            target += GET_JUMP_OFFSET(target);

            fixDoubleTypes(target);
            if (!iterMore(target))
                return Compile_Error;
            PC += JSOP_MOREITER_LENGTH;
            PC += js_CodeSpec[next].length;
            break;
          }
          END_CASE(JSOP_MOREITER)

          BEGIN_CASE(JSOP_ENDITER)
            iterEnd();
          END_CASE(JSOP_ENDITER)

          BEGIN_CASE(JSOP_POP)
            frame.pop();
          END_CASE(JSOP_POP)

          BEGIN_CASE(JSOP_GETARG)
          BEGIN_CASE(JSOP_CALLARG)
          {
            restoreVarType();
            uint32_t arg = GET_SLOTNO(PC);
            if (JSObject *singleton = pushedSingleton(0))
                frame.push(ObjectValue(*singleton));
            else if (script->argsObjAliasesFormals())
                jsop_aliasedArg(arg, /* get = */ true);
            else
                frame.pushArg(arg);
          }
          END_CASE(JSOP_GETARG)

          BEGIN_CASE(JSOP_BINDGNAME)
            jsop_bindgname();
          END_CASE(JSOP_BINDGNAME)

          BEGIN_CASE(JSOP_SETARG)
          {
            jsbytecode *next = &PC[JSOP_SETARG_LENGTH];
            bool pop = JSOp(*next) == JSOP_POP && !analysis->jumpTarget(next);

            uint32_t arg = GET_SLOTNO(PC);
            if (script->argsObjAliasesFormals())
                jsop_aliasedArg(arg, /* get = */ false, pop);
            else
                frame.storeArg(arg, pop);

            updateVarType();

            if (pop) {
                frame.pop();
                PC += JSOP_SETARG_LENGTH + JSOP_POP_LENGTH;
                break;
            }
          }
          END_CASE(JSOP_SETARG)

          BEGIN_CASE(JSOP_GETLOCAL)
          BEGIN_CASE(JSOP_CALLLOCAL)
          BEGIN_CASE(JSOP_GETALIASEDVAR)
          BEGIN_CASE(JSOP_CALLALIASEDVAR)
          {
            /*
             * Update the var type unless we are about to pop the variable.
             * Sync is not guaranteed for types of dead locals, and GETLOCAL
             * followed by POP is not regarded as a use of the variable.
             */
            jsbytecode *next = &PC[JSOP_GETLOCAL_LENGTH];
            if (JSOp(*next) != JSOP_POP || analysis->jumpTarget(next))
                restoreVarType();
            if (JSObject *singleton = pushedSingleton(0))
                frame.push(ObjectValue(*singleton));
            else if (JOF_OPTYPE(*PC) == JOF_SCOPECOORD)
                jsop_aliasedVar(ScopeCoordinate(PC), /* get = */ true);
            else
                frame.pushLocal(GET_SLOTNO(PC));

            PC += GetBytecodeLength(PC);
            break;
          }
          END_CASE(JSOP_GETLOCAL)

          BEGIN_CASE(JSOP_SETLOCAL)
          BEGIN_CASE(JSOP_SETALIASEDVAR)
          {
            jsbytecode *next = &PC[GetBytecodeLength(PC)];
            bool pop = JSOp(*next) == JSOP_POP && !analysis->jumpTarget(next);
            if (JOF_OPTYPE(*PC) == JOF_SCOPECOORD)
                jsop_aliasedVar(ScopeCoordinate(PC), /* get = */ false, pop);
            else
                frame.storeLocal(GET_SLOTNO(PC), pop);
            updateVarType();

            if (pop) {
                frame.pop();
                PC = next + JSOP_POP_LENGTH;
                break;
            }

            PC = next;
            break;
          }
          END_CASE(JSOP_SETLOCAL)

          BEGIN_CASE(JSOP_UINT16)
            frame.push(Value(Int32Value((int32_t) GET_UINT16(PC))));
          END_CASE(JSOP_UINT16)

          BEGIN_CASE(JSOP_NEWINIT)
            if (!jsop_newinit())
                return Compile_Error;
          END_CASE(JSOP_NEWINIT)

          BEGIN_CASE(JSOP_NEWARRAY)
            if (!jsop_newinit())
                return Compile_Error;
          END_CASE(JSOP_NEWARRAY)

          BEGIN_CASE(JSOP_NEWOBJECT)
            if (!jsop_newinit())
                return Compile_Error;
          END_CASE(JSOP_NEWOBJECT)

          BEGIN_CASE(JSOP_ENDINIT)
          END_CASE(JSOP_ENDINIT)

          BEGIN_CASE(JSOP_INITPROP)
            jsop_initprop();
            frame.pop();
          END_CASE(JSOP_INITPROP)

          BEGIN_CASE(JSOP_INITELEM)
            jsop_initelem();
            frame.popn(2);
          END_CASE(JSOP_INITELEM)

          BEGIN_CASE(JSOP_INCARG)
          BEGIN_CASE(JSOP_DECARG)
          BEGIN_CASE(JSOP_ARGINC)
          BEGIN_CASE(JSOP_ARGDEC)
            if (script->hasScriptCounts) {
                restoreVarType();
                FrameEntry *fe = frame.getArg(GET_SLOTNO(PC));
                if (fe->isTypeKnown())
                    arithFirstUseType = fe->getKnownType();
            }

            if (!jsop_arginc(op, GET_SLOTNO(PC)))
                return Compile_Retry;

            if (script->hasScriptCounts) {
                FrameEntry *fe = frame.getArg(GET_SLOTNO(PC));
                updateArithCounts(PC, fe, arithFirstUseType, JSVAL_TYPE_INT32);
                arithUpdated = true;
            }
          END_CASE(JSOP_ARGDEC)

          BEGIN_CASE(JSOP_INCLOCAL)
          BEGIN_CASE(JSOP_DECLOCAL)
          BEGIN_CASE(JSOP_LOCALINC)
          BEGIN_CASE(JSOP_LOCALDEC)
            if (script->hasScriptCounts) {
                restoreVarType();
                FrameEntry *fe = frame.getLocal(GET_SLOTNO(PC));
                if (fe->isTypeKnown())
                    arithFirstUseType = fe->getKnownType();
            }

            if (!jsop_localinc(op, GET_SLOTNO(PC)))
                return Compile_Retry;

            if (script->hasScriptCounts) {
                FrameEntry *fe = frame.getLocal(GET_SLOTNO(PC));
                updateArithCounts(PC, fe, arithFirstUseType, JSVAL_TYPE_INT32);
                arithUpdated = true;
            }
          END_CASE(JSOP_LOCALDEC)

          BEGIN_CASE(JSOP_BINDNAME)
            jsop_bindname(script->getName(GET_UINT32_INDEX(PC)));
          END_CASE(JSOP_BINDNAME)

          BEGIN_CASE(JSOP_SETPROP)
          {
            jsbytecode *next = &PC[JSOP_SETPROP_LENGTH];
            bool pop = JSOp(*next) == JSOP_POP && !analysis->jumpTarget(next);
            if (!jsop_setprop(script->getName(GET_UINT32_INDEX(PC)), pop))
                return Compile_Error;
          }
          END_CASE(JSOP_SETPROP)

          BEGIN_CASE(JSOP_SETNAME)
          {
            jsbytecode *next = &PC[JSOP_SETNAME_LENGTH];
            bool pop = JSOp(*next) == JSOP_POP && !analysis->jumpTarget(next);
            if (!jsop_setprop(script->getName(GET_UINT32_INDEX(PC)), pop))
                return Compile_Error;
          }
          END_CASE(JSOP_SETNAME)

          BEGIN_CASE(JSOP_THROW)
            prepareStubCall(Uses(1));
            INLINE_STUBCALL(stubs::Throw, REJOIN_NONE);
            frame.pop();
            fallthrough = false;
          END_CASE(JSOP_THROW)

          BEGIN_CASE(JSOP_IN)
          {
            jsop_in();
          }
          END_CASE(JSOP_IN)

          BEGIN_CASE(JSOP_INSTANCEOF)
            if (!jsop_instanceof())
                return Compile_Error;
          END_CASE(JSOP_INSTANCEOF)

          BEGIN_CASE(JSOP_EXCEPTION)
          {
            prepareStubCall(Uses(0));
            INLINE_STUBCALL(stubs::Exception, REJOIN_FALLTHROUGH);
            frame.pushSynced(JSVAL_TYPE_UNKNOWN);
          }
          END_CASE(JSOP_EXCEPTION)

          BEGIN_CASE(JSOP_LINENO)
          END_CASE(JSOP_LINENO)

          BEGIN_CASE(JSOP_ENUMELEM)
            // Normally, SETELEM transforms the stack
            //  from: OBJ ID VALUE
            //  to:   VALUE
            //
            // Here, the stack transition is
            //  from: VALUE OBJ ID
            //  to:
            // So we make the stack look like a SETELEM, and re-use it.

            // Before: VALUE OBJ ID
            // After:  VALUE OBJ ID VALUE
            frame.dupAt(-3);

            // Before: VALUE OBJ ID VALUE
            // After:  VALUE VALUE
            if (!jsop_setelem(true))
                return Compile_Error;

            // Before: VALUE VALUE
            // After:
            frame.popn(2);
          END_CASE(JSOP_ENUMELEM)

          BEGIN_CASE(JSOP_CONDSWITCH)
            /* No-op for the decompiler. */
          END_CASE(JSOP_CONDSWITCH)

          BEGIN_CASE(JSOP_LABEL)
          END_CASE(JSOP_LABEL)

          BEGIN_CASE(JSOP_DEFFUN)
          {
            JSFunction *innerFun = script->getFunction(GET_UINT32_INDEX(PC));

            prepareStubCall(Uses(0));
            masm.move(ImmPtr(innerFun), Registers::ArgReg1);
            INLINE_STUBCALL(STRICT_VARIANT(stubs::DefFun), REJOIN_FALLTHROUGH);
          }
          END_CASE(JSOP_DEFFUN)

          BEGIN_CASE(JSOP_DEFVAR)
          BEGIN_CASE(JSOP_DEFCONST)
          {
            PropertyName *name = script->getName(GET_UINT32_INDEX(PC));

            prepareStubCall(Uses(0));
            masm.move(ImmPtr(name), Registers::ArgReg1);
            INLINE_STUBCALL(stubs::DefVarOrConst, REJOIN_FALLTHROUGH);
          }
          END_CASE(JSOP_DEFVAR)

          BEGIN_CASE(JSOP_SETCONST)
          {
            PropertyName *name = script->getName(GET_UINT32_INDEX(PC));

            prepareStubCall(Uses(1));
            masm.move(ImmPtr(name), Registers::ArgReg1);
            INLINE_STUBCALL(stubs::SetConst, REJOIN_FALLTHROUGH);
          }
          END_CASE(JSOP_SETCONST)

          BEGIN_CASE(JSOP_LAMBDA)
          {
            JSFunction *fun = script->getFunction(GET_UINT32_INDEX(PC));

            JSObjStubFun stub = stubs::Lambda;
            uint32_t uses = 0;

            prepareStubCall(Uses(uses));
            masm.move(ImmPtr(fun), Registers::ArgReg1);

            INLINE_STUBCALL(stub, REJOIN_PUSH_OBJECT);

            frame.takeReg(Registers::ReturnReg);
            frame.pushTypedPayload(JSVAL_TYPE_OBJECT, Registers::ReturnReg);
          }
          END_CASE(JSOP_LAMBDA)

          BEGIN_CASE(JSOP_TRY)
            frame.syncAndForgetEverything();
          END_CASE(JSOP_TRY)

          BEGIN_CASE(JSOP_RETRVAL)
            emitReturn(NULL);
            fallthrough = false;
          END_CASE(JSOP_RETRVAL)

          BEGIN_CASE(JSOP_GETGNAME)
          BEGIN_CASE(JSOP_CALLGNAME)
          {
            uint32_t index = GET_UINT32_INDEX(PC);
            if (!jsop_getgname(index))
                return Compile_Error;
            frame.extra(frame.peek(-1)).name = script->getName(index);
          }
          END_CASE(JSOP_GETGNAME)

          BEGIN_CASE(JSOP_SETGNAME)
          {
            jsbytecode *next = &PC[JSOP_SETGNAME_LENGTH];
            bool pop = JSOp(*next) == JSOP_POP && !analysis->jumpTarget(next);
            if (!jsop_setgname(script->getName(GET_UINT32_INDEX(PC)), pop))
                return Compile_Error;
          }
          END_CASE(JSOP_SETGNAME)

          BEGIN_CASE(JSOP_REGEXP)
            if (!jsop_regexp())
                return Compile_Error;
          END_CASE(JSOP_REGEXP)

          BEGIN_CASE(JSOP_OBJECT)
          {
            JSObject *object = script->getObject(GET_UINT32_INDEX(PC));
            RegisterID reg = frame.allocReg();
            masm.move(ImmPtr(object), reg);
            frame.pushTypedPayload(JSVAL_TYPE_OBJECT, reg);
          }
          END_CASE(JSOP_OBJECT)

          BEGIN_CASE(JSOP_UINT24)
            frame.push(Value(Int32Value((int32_t) GET_UINT24(PC))));
          END_CASE(JSOP_UINT24)

          BEGIN_CASE(JSOP_STOP)
            if (script->hasScriptCounts)
                updatePCCounts(PC, &codeStart, &countsUpdated);
            emitReturn(NULL);
            goto done;
          END_CASE(JSOP_STOP)

          BEGIN_CASE(JSOP_GETXPROP)
            if (!jsop_xname(script->getName(GET_UINT32_INDEX(PC))))
                return Compile_Error;
          END_CASE(JSOP_GETXPROP)

          BEGIN_CASE(JSOP_ENTERBLOCK)
          BEGIN_CASE(JSOP_ENTERLET0)
          BEGIN_CASE(JSOP_ENTERLET1)
            enterBlock(&script->getObject(GET_UINT32_INDEX(PC))->asStaticBlock());
          END_CASE(JSOP_ENTERBLOCK);

          BEGIN_CASE(JSOP_LEAVEBLOCK)
            leaveBlock();
          END_CASE(JSOP_LEAVEBLOCK)

          BEGIN_CASE(JSOP_INT8)
            frame.push(Value(Int32Value(GET_INT8(PC))));
          END_CASE(JSOP_INT8)

          BEGIN_CASE(JSOP_INT32)
            frame.push(Value(Int32Value(GET_INT32(PC))));
          END_CASE(JSOP_INT32)

          BEGIN_CASE(JSOP_HOLE)
            frame.push(MagicValue(JS_ARRAY_HOLE));
          END_CASE(JSOP_HOLE)

          BEGIN_CASE(JSOP_LOOPHEAD)
          {
            if (analysis->jumpTarget(PC)) {
                interruptCheckHelper();
                recompileCheckHelper();
            }
          }
          END_CASE(JSOP_LOOPHEAD)

          BEGIN_CASE(JSOP_LOOPENTRY)
          END_CASE(JSOP_LOOPENTRY)

          BEGIN_CASE(JSOP_DEBUGGER)
          {
            prepareStubCall(Uses(0));
            masm.move(ImmPtr(PC), Registers::ArgReg1);
            INLINE_STUBCALL(stubs::DebuggerStatement, REJOIN_FALLTHROUGH);
          }
          END_CASE(JSOP_DEBUGGER)

          default:
            JS_NOT_REACHED("Opcode not implemented");
        }

    /**********************
     *  END COMPILER OPS  *
     **********************/

        if (cx->typeInferenceEnabled() && PC == lastPC + GetBytecodeLength(lastPC)) {
            /*
             * Inform the frame of the type sets for values just pushed. Skip
             * this if we did any opcode fusions, we don't keep track of the
             * associated type sets in such cases.
             */
            unsigned nuses = GetUseCount(script, lastPC - script->code);
            unsigned ndefs = GetDefCount(script, lastPC - script->code);
            for (unsigned i = 0; i < ndefs; i++) {
                FrameEntry *fe = frame.getStack(opinfo->stackDepth - nuses + i);
                if (fe) {
                    /* fe may be NULL for conditionally pushed entries, e.g. JSOP_AND */
                    frame.extra(fe).types = analysis->pushedTypes(lastPC - script->code, i);
                }
            }
        }

        if (script->hasScriptCounts) {
            size_t length = masm.size() - masm.distanceOf(codeStart);
            bool typesUpdated = false;

            /* Update information about the type of value pushed by arithmetic ops. */
            if ((js_CodeSpec[op].format & JOF_ARITH) && !arithUpdated) {
                FrameEntry *pushed = NULL;
                if (PC == lastPC + GetBytecodeLength(lastPC))
                    pushed = frame.peek(-1);
                updateArithCounts(lastPC, pushed, arithFirstUseType, arithSecondUseType);
                typesUpdated = true;
            }

            /* Update information about the result type of access operations. */
            if (PCCounts::accessOp(op) &&
                op != JSOP_SETPROP && op != JSOP_SETELEM) {
                FrameEntry *fe = (GetDefCount(script, lastPC - script->code) == 1)
                    ? frame.peek(-1)
                    : frame.peek(-2);
                updatePCTypes(lastPC, fe);
                typesUpdated = true;
            }

            if (countsUpdated || typesUpdated || length != 0) {
                if (!countsUpdated)
                    updatePCCounts(lastPC, &codeStart, &countsUpdated);

                if (pcLengths) {
                    /* Fill in the amount of inline code generated for the op. */
                    uint32_t offset = ssa.frameLength(a->inlineIndex) + lastPC - script->code;
                    pcLengths[offset].codeLength += length;
                }
            }
        } else if (pcLengths) {
            /* Fill in the amount of inline code generated for the op. */
            size_t length = masm.size() - masm.distanceOf(codeStart);
            uint32_t offset = ssa.frameLength(a->inlineIndex) + lastPC - script->code;
            pcLengths[offset].codeLength += length;
        }

        frame.assertValidRegisterState();
    }

  done:
    return Compile_Okay;
}

#undef END_CASE
#undef BEGIN_CASE

void
mjit::Compiler::updatePCCounts(jsbytecode *pc, Label *start, bool *updated)
{
    JS_ASSERT(script->hasScriptCounts);

    /*
     * Bump the METHODJIT count for the opcode, read the METHODJIT_CODE_LENGTH
     * and METHODJIT_PICS_LENGTH counts, indicating the amounts of inline path
     * code and generated code, respectively, and add them to the accumulated
     * total for the op.
     */
    uint32_t offset = ssa.frameLength(a->inlineIndex) + pc - script->code;

    /*
     * Base register for addresses, we can't use AbsoluteAddress in all places.
     * This may hold a live value, so write it out to the top of the stack
     * first. This cannot overflow the stack, as space is always reserved for
     * an extra callee frame.
     */
    RegisterID reg = Registers::ReturnReg;
    masm.storePtr(reg, frame.addressOfTop());

    PCCounts counts = script->getPCCounts(pc);

    double *code = &counts.get(PCCounts::BASE_METHODJIT_CODE);
    double *codeLength = &pcLengths[offset].codeLength;
    masm.addCount(codeLength, code, reg);

    double *pics = &counts.get(PCCounts::BASE_METHODJIT_PICS);
    double *picsLength = &pcLengths[offset].picsLength;
    masm.addCount(picsLength, pics, reg);

    double *count = &counts.get(PCCounts::BASE_METHODJIT);
    masm.bumpCount(count, reg);

    /* Reload the base register's original value. */
    masm.loadPtr(frame.addressOfTop(), reg);

    /* The start label should reflect the code for the op, not instrumentation. */
    *start = masm.label();
    *updated = true;
}

static inline bool
HasPayloadType(types::TypeSet *types)
{
    if (types->unknown())
        return false;

    types::TypeFlags flags = types->baseFlags();
    bool objects = !!(flags & types::TYPE_FLAG_ANYOBJECT) || !!types->getObjectCount();

    if (objects && !!(flags & types::TYPE_FLAG_STRING))
        return false;

    flags = flags & ~(types::TYPE_FLAG_ANYOBJECT | types::TYPE_FLAG_STRING);

    return (flags == types::TYPE_FLAG_UNDEFINED)
        || (flags == types::TYPE_FLAG_NULL)
        || (flags == types::TYPE_FLAG_BOOLEAN);
}

void
mjit::Compiler::updatePCTypes(jsbytecode *pc, FrameEntry *fe)
{
    JS_ASSERT(script->hasScriptCounts);

    /*
     * Get a temporary register, as for updatePCCounts. Don't overlap with
     * the backing store for the entry's type tag, if there is one.
     */
    RegisterID reg = Registers::ReturnReg;
    if (frame.peekTypeInRegister(fe) && reg == frame.tempRegForType(fe)) {
        JS_STATIC_ASSERT(Registers::ReturnReg != Registers::ArgReg1);
        reg = Registers::ArgReg1;
    }
    masm.push(reg);

    PCCounts counts = script->getPCCounts(pc);

    /* Update the counts for pushed type tags and possible access types. */
    if (fe->isTypeKnown()) {
        masm.bumpCount(&counts.get(PCCounts::ACCESS_MONOMORPHIC), reg);
        PCCounts::AccessCounts count = PCCounts::ACCESS_OBJECT;
        switch (fe->getKnownType()) {
          case JSVAL_TYPE_UNDEFINED:  count = PCCounts::ACCESS_UNDEFINED;  break;
          case JSVAL_TYPE_NULL:       count = PCCounts::ACCESS_NULL;       break;
          case JSVAL_TYPE_BOOLEAN:    count = PCCounts::ACCESS_BOOLEAN;    break;
          case JSVAL_TYPE_INT32:      count = PCCounts::ACCESS_INT32;      break;
          case JSVAL_TYPE_DOUBLE:     count = PCCounts::ACCESS_DOUBLE;     break;
          case JSVAL_TYPE_STRING:     count = PCCounts::ACCESS_STRING;     break;
          case JSVAL_TYPE_OBJECT:     count = PCCounts::ACCESS_OBJECT;     break;
          default:;
        }
        if (count)
            masm.bumpCount(&counts.get(count), reg);
    } else {
        types::TypeSet *types = frame.extra(fe).types;
        if (types && HasPayloadType(types))
            masm.bumpCount(&counts.get(PCCounts::ACCESS_DIMORPHIC), reg);
        else
            masm.bumpCount(&counts.get(PCCounts::ACCESS_POLYMORPHIC), reg);

        frame.loadTypeIntoReg(fe, reg);

        Jump j = masm.testUndefined(Assembler::NotEqual, reg);
        masm.bumpCount(&counts.get(PCCounts::ACCESS_UNDEFINED), reg);
        frame.loadTypeIntoReg(fe, reg);
        j.linkTo(masm.label(), &masm);

        j = masm.testNull(Assembler::NotEqual, reg);
        masm.bumpCount(&counts.get(PCCounts::ACCESS_NULL), reg);
        frame.loadTypeIntoReg(fe, reg);
        j.linkTo(masm.label(), &masm);

        j = masm.testBoolean(Assembler::NotEqual, reg);
        masm.bumpCount(&counts.get(PCCounts::ACCESS_BOOLEAN), reg);
        frame.loadTypeIntoReg(fe, reg);
        j.linkTo(masm.label(), &masm);

        j = masm.testInt32(Assembler::NotEqual, reg);
        masm.bumpCount(&counts.get(PCCounts::ACCESS_INT32), reg);
        frame.loadTypeIntoReg(fe, reg);
        j.linkTo(masm.label(), &masm);

        j = masm.testDouble(Assembler::NotEqual, reg);
        masm.bumpCount(&counts.get(PCCounts::ACCESS_DOUBLE), reg);
        frame.loadTypeIntoReg(fe, reg);
        j.linkTo(masm.label(), &masm);

        j = masm.testString(Assembler::NotEqual, reg);
        masm.bumpCount(&counts.get(PCCounts::ACCESS_STRING), reg);
        frame.loadTypeIntoReg(fe, reg);
        j.linkTo(masm.label(), &masm);

        j = masm.testObject(Assembler::NotEqual, reg);
        masm.bumpCount(&counts.get(PCCounts::ACCESS_OBJECT), reg);
        frame.loadTypeIntoReg(fe, reg);
        j.linkTo(masm.label(), &masm);
    }

    /* Update the count for accesses with type barriers. */
    if (js_CodeSpec[*pc].format & JOF_TYPESET) {
        double *count = &counts.get(hasTypeBarriers(pc)
                                      ? PCCounts::ACCESS_BARRIER
                                      : PCCounts::ACCESS_NOBARRIER);
        masm.bumpCount(count, reg);
    }

    /* Reload the base register's original value. */
    masm.pop(reg);
}

void
mjit::Compiler::updateArithCounts(jsbytecode *pc, FrameEntry *fe,
                                  JSValueType firstUseType, JSValueType secondUseType)
{
    JS_ASSERT(script->hasScriptCounts);

    RegisterID reg = Registers::ReturnReg;
    masm.push(reg);

    /*
     * What count we bump for arithmetic expressions depend on the
     * known types of its operands.
     *
     * ARITH_INT: operands are known ints, result is int
     * ARITH_OVERFLOW: operands are known ints, result is double
     * ARITH_DOUBLE: either operand is a known double, result is double
     * ARITH_OTHER: operands are monomorphic but not int or double
     * ARITH_UNKNOWN: operands are polymorphic
     */

    PCCounts::ArithCounts count;
    if (firstUseType == JSVAL_TYPE_INT32 && secondUseType == JSVAL_TYPE_INT32 &&
        (!fe || fe->isNotType(JSVAL_TYPE_DOUBLE))) {
        count = PCCounts::ARITH_INT;
    } else if (firstUseType == JSVAL_TYPE_INT32 || firstUseType == JSVAL_TYPE_DOUBLE ||
               secondUseType == JSVAL_TYPE_INT32 || secondUseType == JSVAL_TYPE_DOUBLE) {
        count = PCCounts::ARITH_DOUBLE;
    } else if (firstUseType != JSVAL_TYPE_UNKNOWN && secondUseType != JSVAL_TYPE_UNKNOWN &&
               (!fe || fe->isTypeKnown())) {
        count = PCCounts::ARITH_OTHER;
    } else {
        count = PCCounts::ARITH_UNKNOWN;
    }

    masm.bumpCount(&script->getPCCounts(pc).get(count), reg);
    masm.pop(reg);
}

void
mjit::Compiler::updateElemCounts(jsbytecode *pc, FrameEntry *obj, FrameEntry *id)
{
    JS_ASSERT(script->hasScriptCounts);

    RegisterID reg = Registers::ReturnReg;
    masm.push(reg);

    PCCounts counts = script->getPCCounts(pc);

    PCCounts::ElementCounts count;
    if (id->isTypeKnown()) {
        switch (id->getKnownType()) {
          case JSVAL_TYPE_INT32:   count = PCCounts::ELEM_ID_INT;     break;
          case JSVAL_TYPE_DOUBLE:  count = PCCounts::ELEM_ID_DOUBLE;  break;
          default:                 count = PCCounts::ELEM_ID_OTHER;   break;
        }
    } else {
        count = PCCounts::ELEM_ID_UNKNOWN;
    }
    masm.bumpCount(&counts.get(count), reg);

    if (obj->mightBeType(JSVAL_TYPE_OBJECT)) {
        types::TypeSet *types = frame.extra(obj).types;
        if (types && !types->hasObjectFlags(cx, types::OBJECT_FLAG_NON_TYPED_ARRAY) &&
            types->getTypedArrayType(cx) != TypedArray::TYPE_MAX) {
            count = PCCounts::ELEM_OBJECT_TYPED;
        } else if (types && !types->hasObjectFlags(cx, types::OBJECT_FLAG_NON_DENSE_ARRAY)) {
            if (!types->hasObjectFlags(cx, types::OBJECT_FLAG_NON_PACKED_ARRAY))
                count = PCCounts::ELEM_OBJECT_PACKED;
            else
                count = PCCounts::ELEM_OBJECT_DENSE;
        } else {
            count = PCCounts::ELEM_OBJECT_OTHER;
        }
        masm.bumpCount(&counts.get(count), reg);
    } else {
        masm.bumpCount(&counts.get(PCCounts::ELEM_OBJECT_OTHER), reg);
    }

    masm.pop(reg);
}

void
mjit::Compiler::bumpPropCount(jsbytecode *pc, int count)
{
    /* Don't accumulate counts for property ops fused with other ops. */
    if (!(js_CodeSpec[*pc].format & JOF_PROP))
        return;
    RegisterID reg = Registers::ReturnReg;
    masm.push(reg);
    masm.bumpCount(&script->getPCCounts(pc).get(count), reg);
    masm.pop(reg);
}

JSC::MacroAssembler::Label
mjit::Compiler::labelOf(jsbytecode *pc, uint32_t inlineIndex)
{
    ActiveFrame *a = (inlineIndex == UINT32_MAX) ? outer : inlineFrames[inlineIndex];
    JS_ASSERT(uint32_t(pc - a->script->code) < a->script->length);

    uint32_t offs = uint32_t(pc - a->script->code);
    JS_ASSERT(a->jumpMap[offs].isSet());
    return a->jumpMap[offs];
}

bool
mjit::Compiler::knownJump(jsbytecode *pc)
{
    return pc < PC;
}

bool
mjit::Compiler::jumpInScript(Jump j, jsbytecode *pc)
{
    JS_ASSERT(pc >= script->code && uint32_t(pc - script->code) < script->length);

    if (pc < PC) {
        j.linkTo(a->jumpMap[uint32_t(pc - script->code)], &masm);
        return true;
    }
    return branchPatches.append(BranchPatch(j, pc, a->inlineIndex));
}

void
mjit::Compiler::emitFinalReturn(Assembler &masm)
{
    masm.loadPtr(Address(JSFrameReg, StackFrame::offsetOfNcode()), Registers::ReturnReg);
    masm.jump(Registers::ReturnReg);
}

// Emits code to load a return value of the frame into the scripted-ABI
// type & data register pair. If the return value is in fp->rval, then |fe|
// is NULL. Otherwise, |fe| contains the return value.
//
// If reading from fp->rval, |undefined| is loaded optimistically, before
// checking if fp->rval is set in the frame flags and loading that instead.
//
// Otherwise, if |masm| is the inline path, it is loaded as efficiently as
// the FrameState can manage. If |masm| is the OOL path, the value is simply
// loaded from its slot in the frame, since the caller has guaranteed it's
// been synced.
//
void
mjit::Compiler::loadReturnValue(Assembler *masm, FrameEntry *fe)
{
    RegisterID typeReg = JSReturnReg_Type;
    RegisterID dataReg = JSReturnReg_Data;

    if (fe) {
        // If using the OOL assembler, the caller signifies that the |fe| is
        // synced, but not to rely on its register state.
        if (masm != &this->masm) {
            if (fe->isConstant()) {
                stubcc.masm.loadValueAsComponents(fe->getValue(), typeReg, dataReg);
            } else {
                Address rval(frame.addressOf(fe));
                if (fe->isTypeKnown() && !fe->isType(JSVAL_TYPE_DOUBLE)) {
                    stubcc.masm.loadPayload(rval, dataReg);
                    stubcc.masm.move(ImmType(fe->getKnownType()), typeReg);
                } else {
                    stubcc.masm.loadValueAsComponents(rval, typeReg, dataReg);
                }
            }
        } else {
            frame.loadForReturn(fe, typeReg, dataReg, Registers::ReturnReg);
        }
    } else {
         // Load a return value from POPV or SETRVAL into the return registers,
         // otherwise return undefined.
        masm->loadValueAsComponents(UndefinedValue(), typeReg, dataReg);
        if (analysis->usesReturnValue()) {
            Jump rvalClear = masm->branchTest32(Assembler::Zero,
                                               FrameFlagsAddress(),
                                               Imm32(StackFrame::HAS_RVAL));
            Address rvalAddress(JSFrameReg, StackFrame::offsetOfReturnValue());
            masm->loadValueAsComponents(rvalAddress, typeReg, dataReg);
            rvalClear.linkTo(masm->label(), masm);
        }
    }
}

// This ensures that constructor return values are an object. If a non-object
// is returned, either explicitly or implicitly, the newly created object is
// loaded out of the frame. Otherwise, the explicitly returned object is kept.
//
void
mjit::Compiler::fixPrimitiveReturn(Assembler *masm, FrameEntry *fe)
{
    JS_ASSERT(isConstructing);

    bool ool = (masm != &this->masm);
    Address thisv(JSFrameReg, StackFrame::offsetOfThis(script->function()));

    // We can just load |thisv| if either of the following is true:
    //  (1) There is no explicit return value, AND fp->rval is not used.
    //  (2) There is an explicit return value, and it's known to be primitive.
    if ((!fe && !analysis->usesReturnValue()) ||
        (fe && fe->isTypeKnown() && fe->getKnownType() != JSVAL_TYPE_OBJECT))
    {
        if (ool)
            masm->loadValueAsComponents(thisv, JSReturnReg_Type, JSReturnReg_Data);
        else
            frame.loadThisForReturn(JSReturnReg_Type, JSReturnReg_Data, Registers::ReturnReg);
        return;
    }

    // If the type is known to be an object, just load the return value as normal.
    if (fe && fe->isTypeKnown() && fe->getKnownType() == JSVAL_TYPE_OBJECT) {
        loadReturnValue(masm, fe);
        return;
    }

    // There's a return value, and its type is unknown. Test the type and load
    // |thisv| if necessary. Sync the 'this' entry before doing so, as it may
    // be stored in registers if we constructed it inline.
    frame.syncThis();
    loadReturnValue(masm, fe);
    Jump j = masm->testObject(Assembler::Equal, JSReturnReg_Type);
    masm->loadValueAsComponents(thisv, JSReturnReg_Type, JSReturnReg_Data);
    j.linkTo(masm->label(), masm);
}

// Loads the return value into the scripted ABI register pair, such that JS
// semantics in constructors are preserved.
//
void
mjit::Compiler::emitReturnValue(Assembler *masm, FrameEntry *fe)
{
    if (isConstructing)
        fixPrimitiveReturn(masm, fe);
    else
        loadReturnValue(masm, fe);
}

void
mjit::Compiler::emitInlineReturnValue(FrameEntry *fe)
{
    JS_ASSERT(!isConstructing && a->needReturnValue);

    if (a->syncReturnValue) {
        /* Needed return value with unknown type, the caller's entry is synced. */
        Address address = frame.addressForInlineReturn();
        if (fe)
            frame.storeTo(fe, address);
        else
            masm.storeValue(UndefinedValue(), address);
        return;
    }

    /*
     * For inlined functions that simply return an entry present in the outer
     * script (e.g. a loop invariant term), mark the copy and propagate it
     * after popping the frame.
     */
    if (!a->exitState && fe && fe->isCopy() && frame.isOuterSlot(fe->backing())) {
        a->returnEntry = fe->backing();
        return;
    }

    if (a->returnValueDouble) {
        JS_ASSERT(fe);
        frame.ensureDouble(fe);
        Registers mask(a->returnSet
                       ? Registers::maskReg(a->returnRegister)
                       : Registers::AvailFPRegs);
        FPRegisterID fpreg;
        if (!fe->isConstant()) {
            fpreg = frame.tempRegInMaskForData(fe, mask.freeMask).fpreg();
            frame.syncAndForgetFe(fe, true);
            frame.takeReg(fpreg);
        } else {
            fpreg = frame.allocReg(mask.freeMask).fpreg();
            masm.slowLoadConstantDouble(fe->getValue().toDouble(), fpreg);
        }
        JS_ASSERT_IF(a->returnSet, fpreg == a->returnRegister.fpreg());
        a->returnRegister = fpreg;
    } else {
        Registers mask(a->returnSet
                       ? Registers::maskReg(a->returnRegister)
                       : Registers::AvailRegs);
        RegisterID reg;
        if (fe && !fe->isConstant()) {
            reg = frame.tempRegInMaskForData(fe, mask.freeMask).reg();
            frame.syncAndForgetFe(fe, true);
            frame.takeReg(reg);
        } else {
            reg = frame.allocReg(mask.freeMask).reg();
            Value val = fe ? fe->getValue() : UndefinedValue();
            masm.loadValuePayload(val, reg);
        }
        JS_ASSERT_IF(a->returnSet, reg == a->returnRegister.reg());
        a->returnRegister = reg;
    }

    a->returnSet = true;
    if (a->exitState)
        a->exitState->setUnassigned(a->returnRegister);
}

void
mjit::Compiler::emitReturn(FrameEntry *fe)
{
    JS_ASSERT_IF(!script->function(), JSOp(*PC) == JSOP_STOP);

    /* Only the top of the stack can be returned. */
    JS_ASSERT_IF(fe, fe == frame.peek(-1));

    if (debugMode()) {
        /* If the return value isn't in the frame's rval slot, move it there. */
        if (fe) {
            frame.storeTo(fe, Address(JSFrameReg, StackFrame::offsetOfReturnValue()), true);

            /* Set the frame flag indicating it's there. */
            RegisterID reg = frame.allocReg();
            masm.load32(FrameFlagsAddress(), reg);
            masm.or32(Imm32(StackFrame::HAS_RVAL), reg);
            masm.store32(reg, FrameFlagsAddress());
            frame.freeReg(reg);

            /* Use the frame's return value when generating further code. */
            fe = NULL;
        }

        prepareStubCall(Uses(0));
        INLINE_STUBCALL(stubs::ScriptDebugEpilogue, REJOIN_RESUME);
    }

    if (a != outer) {
        JS_ASSERT(!debugMode());
        profilingPopHelper();

        /*
         * Returning from an inlined script. The checks we do for inlineability
         * and recompilation triggered by args object construction ensure that
         * there can't be an arguments or call object.
         */

        if (a->needReturnValue)
            emitInlineReturnValue(fe);

        if (a->exitState) {
            /*
             * Restore the register state to reflect that at the original call,
             * modulo entries which will be popped once the call finishes and any
             * entry which will be clobbered by the return value register.
             */
            frame.syncForAllocation(a->exitState, true, Uses(0));
        }

        /*
         * Simple tests to see if we are at the end of the script and will
         * fallthrough after the script body finishes, thus won't need to jump.
         */
        bool endOfScript =
            (JSOp(*PC) == JSOP_STOP) ||
            (JSOp(*PC) == JSOP_RETURN &&
             (JSOp(PC[JSOP_RETURN_LENGTH]) == JSOP_STOP &&
              !analysis->maybeCode(PC + JSOP_RETURN_LENGTH)));
        if (!endOfScript)
            a->returnJumps->append(masm.jump());

        if (a->returnSet)
            frame.freeReg(a->returnRegister);
        return;
    }

    /* Inline StackFrame::epilogue. */
    if (debugMode()) {
        prepareStubCall(Uses(0));
        INLINE_STUBCALL(stubs::Epilogue, REJOIN_NONE);
    } else {
        profilingPopHelper();

        if (script->function() && script->nesting()) {
            masm.sub32(Imm32(1), AbsoluteAddress(&script->nesting()->activeFrames));
        }
    }

    emitReturnValue(&masm, fe);
    emitFinalReturn(masm);

    /*
     * After we've placed the call object, all tracked state can be
     * thrown away. This will happen anyway because the next live opcode (if
     * any) must have an incoming edge. It's an optimization to throw it away
     * early - the tracker won't be spilled on further exits or join points.
     */
    frame.discardFrame();
}

void
mjit::Compiler::prepareStubCall(Uses uses)
{
    JaegerSpew(JSpew_Insns, " ---- STUB CALL, SYNCING FRAME ---- \n");
    frame.syncAndKill(Registers(Registers::TempAnyRegs), uses);
    JaegerSpew(JSpew_Insns, " ---- FRAME SYNCING DONE ---- \n");
}

JSC::MacroAssembler::Call
mjit::Compiler::emitStubCall(void *ptr, DataLabelPtr *pinline)
{
    JaegerSpew(JSpew_Insns, " ---- CALLING STUB ---- \n");

    masm.bumpStubCount(script, PC, Registers::tempCallReg());

    Call cl = masm.fallibleVMCall(cx->typeInferenceEnabled(),
                                  ptr, outerPC(), pinline, frame.totalDepth());
    JaegerSpew(JSpew_Insns, " ---- END STUB CALL ---- \n");
    return cl;
}

void
mjit::Compiler::interruptCheckHelper()
{
    Jump jump;
    if (cx->runtime->gcZeal() == js::gc::ZealVerifierValue) {
        /* For barrier verification, always take the interrupt so we can verify. */
        jump = masm.jump();
    } else {
        void *interrupt = (void*) &cx->runtime->interrupt;
#if defined(JS_CPU_X86) || defined(JS_CPU_ARM) || defined(JS_CPU_MIPS)
        jump = masm.branch32(Assembler::NotEqual, AbsoluteAddress(interrupt), Imm32(0));
#else
        /* Handle processors that can't load from absolute addresses. */
        RegisterID reg = frame.allocReg();
        masm.move(ImmPtr(interrupt), reg);
        jump = masm.branchTest32(Assembler::NonZero, Address(reg, 0));
        frame.freeReg(reg);
#endif
    }

    stubcc.linkExitDirect(jump, stubcc.masm.label());

    frame.sync(stubcc.masm, Uses(0));
    stubcc.masm.move(ImmPtr(PC), Registers::ArgReg1);
    OOL_STUBCALL(stubs::Interrupt, REJOIN_RESUME);
    stubcc.rejoin(Changes(0));
}

static bool
MaybeIonCompileable(JSContext *cx, JSScript *script)
{
#ifdef JS_ION
    if (!ion::IsEnabled(cx))
        return false;
    if (!script->canIonCompile())
        return false;
    return true;
#endif
    return false;
}

void
mjit::Compiler::recompileCheckHelper()
{
    if (inlining() || debugMode() || !globalObj || !cx->typeInferenceEnabled())
        return;

    // Insert a recompile check if either:
    // 1) IonMonkey is enabled, to optimize the function when it becomes hot.
    // 2) The script contains function calls JM can inline.
    if (!MaybeIonCompileable(cx, outerScript) && !analysis->hasFunctionCalls())
        return;

    uint32_t *addr = script->addressOfUseCount();
    masm.add32(Imm32(1), AbsoluteAddress(addr));
#if defined(JS_CPU_X86) || defined(JS_CPU_ARM)
    Jump jump = masm.branch32(Assembler::GreaterThanOrEqual, AbsoluteAddress(addr),
                              Imm32(USES_BEFORE_INLINING));
#else
    /* Handle processors that can't load from absolute addresses. */
    RegisterID reg = frame.allocReg();
    masm.move(ImmPtr(addr), reg);
    Jump jump = masm.branch32(Assembler::GreaterThanOrEqual, Address(reg, 0),
                              Imm32(USES_BEFORE_INLINING));
    frame.freeReg(reg);
#endif
    stubcc.linkExit(jump, Uses(0));
    stubcc.leave();

    OOL_STUBCALL(stubs::RecompileForInline, REJOIN_RESUME);
    stubcc.rejoin(Changes(0));
}

CompileStatus
mjit::Compiler::methodEntryHelper()
{
    if (debugMode()) {
        prepareStubCall(Uses(0));
        INLINE_STUBCALL(stubs::ScriptDebugPrologue, REJOIN_RESUME);
    } else if (Probes::callTrackingActive(cx)) {
        prepareStubCall(Uses(0));
        INLINE_STUBCALL(stubs::ScriptProbeOnlyPrologue, REJOIN_RESUME);
    } else {
        return profilingPushHelper();
    }
    return Compile_Okay;
}

CompileStatus
mjit::Compiler::profilingPushHelper()
{
    SPSProfiler *p = &cx->runtime->spsProfiler;
    if (!p->enabled())
        return Compile_Okay;
    /* If allocation fails, make sure no PopHelper() is emitted */
    const char *str = p->profileString(cx, script, script->function());
    if (str == NULL)
        return Compile_Error;

    /* Check if there's still space on the stack */
    RegisterID size = frame.allocReg();
    RegisterID base = frame.allocReg();
    masm.load32(p->size(), size);
    Jump j = masm.branch32(Assembler::GreaterThanOrEqual, size,
                           Imm32(p->maxSize()));

    /* With room, store our string onto the stack */
    masm.move(ImmPtr(p->stack()), base);
    JS_STATIC_ASSERT(sizeof(ProfileEntry) == 2 * sizeof(void*));
    masm.lshift32(Imm32(sizeof(void*) == 4 ? 3 : 4), size);
    masm.addPtr(size, base);

    masm.storePtr(ImmPtr(str), Address(base, offsetof(ProfileEntry, string)));
    masm.storePtr(ImmPtr(NULL), Address(base, offsetof(ProfileEntry, sp)));

    frame.freeReg(base);
    frame.freeReg(size);

    /* Always increment the stack size (paired with a decrement below) */
    j.linkTo(masm.label(), &masm);
    masm.add32(Imm32(1), AbsoluteAddress(p->size()));

    /* Set the flags that we've pushed information onto the SPS stack */
    RegisterID reg = frame.allocReg();
    masm.load32(FrameFlagsAddress(), reg);
    masm.or32(Imm32(StackFrame::HAS_PUSHED_SPS_FRAME), reg);
    masm.store32(reg, FrameFlagsAddress());
    frame.freeReg(reg);

    return Compile_Okay;
}

void
mjit::Compiler::profilingPopHelper()
{
    if (Probes::callTrackingActive(cx) ||
        cx->runtime->spsProfiler.slowAssertionsEnabled())
    {
        prepareStubCall(Uses(0));
        INLINE_STUBCALL(stubs::ScriptProbeOnlyEpilogue, REJOIN_RESUME);
    } else if (cx->runtime->spsProfiler.enabled()) {
        masm.sub32(Imm32(1), AbsoluteAddress(cx->runtime->spsProfiler.size()));
    }
}

void
mjit::Compiler::addReturnSite()
{
    InternalCallSite site(masm.distanceOf(masm.label()), a->inlineIndex, PC,
                          REJOIN_SCRIPTED, false);
    addCallSite(site);
    masm.loadPtr(Address(JSFrameReg, StackFrame::offsetOfPrev()), JSFrameReg);
}

void
mjit::Compiler::emitUncachedCall(uint32_t argc, bool callingNew)
{
    CallPatchInfo callPatch;

    RegisterID r0 = Registers::ReturnReg;
    VoidPtrStubUInt32 stub = callingNew ? stubs::UncachedNew : stubs::UncachedCall;

    frame.syncAndKill(Uses(argc + 2));
    prepareStubCall(Uses(argc + 2));
    masm.move(Imm32(argc), Registers::ArgReg1);
    INLINE_STUBCALL(stub, REJOIN_CALL_PROLOGUE);

    Jump notCompiled = masm.branchTestPtr(Assembler::Zero, r0, r0);

    masm.loadPtr(FrameAddress(VMFrame::offsetOfRegsSp()), JSFrameReg);
    callPatch.hasFastNcode = true;
    callPatch.fastNcodePatch =
        masm.storePtrWithPatch(ImmPtr(NULL),
                               Address(JSFrameReg, StackFrame::offsetOfNcode()));

    masm.jump(r0);
    callPatch.joinPoint = masm.label();
    addReturnSite();

    frame.popn(argc + 2);

    frame.takeReg(JSReturnReg_Type);
    frame.takeReg(JSReturnReg_Data);
    frame.pushRegs(JSReturnReg_Type, JSReturnReg_Data, knownPushedType(0));

    BarrierState barrier = testBarrier(JSReturnReg_Type, JSReturnReg_Data,
                                       /* testUndefined = */ false,
                                       /* testReturn = */ true);

    stubcc.linkExitDirect(notCompiled, stubcc.masm.label());
    stubcc.rejoin(Changes(1));
    callPatches.append(callPatch);

    finishBarrier(barrier, REJOIN_FALLTHROUGH, 0);
}

void
mjit::Compiler::checkCallApplySpeculation(uint32_t argc, FrameEntry *origCallee, FrameEntry *origThis,
                                          MaybeRegisterID origCalleeType, RegisterID origCalleeData,
                                          MaybeRegisterID origThisType, RegisterID origThisData,
                                          Jump *uncachedCallSlowRejoin, CallPatchInfo *uncachedCallPatch)
{
    JS_ASSERT(IsLowerableFunCallOrApply(PC));

    RegisterID temp;
    Registers tempRegs(Registers::AvailRegs);
    if (origCalleeType.isSet())
        tempRegs.takeReg(origCalleeType.reg());
    tempRegs.takeReg(origCalleeData);
    if (origThisType.isSet())
        tempRegs.takeReg(origThisType.reg());
    tempRegs.takeReg(origThisData);
    temp = tempRegs.takeAnyReg().reg();

    /*
     * if (origCallee.isObject() &&
     *     origCallee.toObject().isFunction &&
     *     origCallee.toObject().toFunction() == js_fun_{call,apply})
     */
    MaybeJump isObj;
    if (origCalleeType.isSet())
        isObj = masm.testObject(Assembler::NotEqual, origCalleeType.reg());
    Jump isFun = masm.testFunction(Assembler::NotEqual, origCalleeData, temp);
    Native native = *PC == JSOP_FUNCALL ? js_fun_call : js_fun_apply;
    Jump isNative = masm.branchPtr(Assembler::NotEqual,
                                   Address(origCalleeData, JSFunction::offsetOfNativeOrScript()),
                                   ImmPtr(JS_FUNC_TO_DATA_PTR(void *, native)));

    /*
     * If speculation fails, we can't use the ic, since it is compiled on the
     * assumption that speculation succeeds. Instead, just do an uncached call.
     */
    {
        if (isObj.isSet())
            stubcc.linkExitDirect(isObj.getJump(), stubcc.masm.label());
        stubcc.linkExitDirect(isFun, stubcc.masm.label());
        stubcc.linkExitDirect(isNative, stubcc.masm.label());

        stubcc.masm.move(Imm32(argc), Registers::ArgReg1);
        JaegerSpew(JSpew_Insns, " ---- BEGIN SLOW CALL CODE ---- \n");
        OOL_STUBCALL_LOCAL_SLOTS(JS_FUNC_TO_DATA_PTR(void *, stubs::SlowCall),
                                 REJOIN_FALLTHROUGH, frame.totalDepth());
        JaegerSpew(JSpew_Insns, " ---- END SLOW CALL CODE ---- \n");

        /*
         * inlineCallHelper will link uncachedCallSlowRejoin to the join point
         * at the end of the ic. At that join point, the return value of the
         * call is assumed to be in registers, so load them before jumping.
         */
        JaegerSpew(JSpew_Insns, " ---- BEGIN SLOW RESTORE CODE ---- \n");
        Address rval = frame.addressOf(origCallee);  /* vp[0] == rval */
        if (knownPushedType(0) == JSVAL_TYPE_DOUBLE)
            stubcc.masm.ensureInMemoryDouble(rval);
        stubcc.masm.loadValueAsComponents(rval, JSReturnReg_Type, JSReturnReg_Data);
        *uncachedCallSlowRejoin = stubcc.masm.jump();
        JaegerSpew(JSpew_Insns, " ---- END SLOW RESTORE CODE ---- \n");
    }
}

/* See MonoIC.cpp, CallCompiler for more information on call ICs. */
bool
mjit::Compiler::inlineCallHelper(uint32_t argc, bool callingNew, FrameSize &callFrameSize)
{
    /*
     * Check for interrupts on function call. We don't do this for lazy
     * arguments objects as the interrupt may kick this frame into the
     * interpreter, which doesn't know about the apply tricks. Instead, we
     * do the interrupt check at the start of the JSOP_ARGUMENTS.
     */
    interruptCheckHelper();

    FrameEntry *origCallee = frame.peek(-(int(argc) + 2));
    FrameEntry *origThis = frame.peek(-(int(argc) + 1));

    /*
     * 'this' does not need to be synced for constructing. :FIXME: is it
     * possible that one of the arguments is directly copying the 'this'
     * entry (something like 'new x.f(x)')?
     */
    if (callingNew) {
        frame.discardFe(origThis);

        /*
         * We store NULL here to ensure that the slot doesn't contain
         * garbage. Additionally, we need to store a non-object value here for
         * TI. If a GC gets triggered before the callee can fill in the slot
         * (i.e. the GC happens on constructing the 'new' object or the call
         * object for a heavyweight callee), it needs to be able to read the
         * 'this' value to tell whether newScript constraints will need to be
         * regenerated afterwards.
         */
        masm.storeValue(NullValue(), frame.addressOf(origThis));
    }

    /*
     * From the presence of JSOP_FUN{CALL,APPLY}, we speculate that we are
     * going to call js_fun_{call,apply}. Normally, this call would go through
     * js::Invoke to ultimately call 'this'. We can do much better by having
     * the callIC cache and call 'this' directly. However, if it turns out that
     * we are not actually calling js_fun_call, the callIC must act as normal.
     *
     * Note: do *NOT* use type information or inline state in any way when
     * deciding whether to lower a CALL or APPLY. The stub calls here store
     * their return values in a different slot, so when recompiling we need
     * to go down the exact same path.
     */
    bool lowerFunCallOrApply = IsLowerableFunCallOrApply(PC);

    bool newType = callingNew && cx->typeInferenceEnabled() && types::UseNewType(cx, script, PC);

#ifdef JS_MONOIC
    if (debugMode() || newType) {
#endif
        emitUncachedCall(argc, callingNew);
        return true;
#ifdef JS_MONOIC
    }

    frame.forgetMismatchedObject(origCallee);
    if (lowerFunCallOrApply)
        frame.forgetMismatchedObject(origThis);

    /* Initialized by both branches below. */
    CallGenInfo     callIC;
    CallPatchInfo   callPatch;
    MaybeRegisterID icCalleeType; /* type to test for function-ness */
    RegisterID      icCalleeData; /* data to call */
    Address         icRvalAddr;   /* return slot on slow-path rejoin */

    /*
     * IC space must be reserved (using RESERVE_IC_SPACE or RESERVE_OOL_SPACE) between the
     * following labels (as used in finishThisUp):
     *  - funGuard -> hotJump
     *  - funGuard -> joinPoint
     *  - funGuard -> hotPathLabel
     *  - slowPathStart -> oolCall
     *  - slowPathStart -> oolJump
     *  - slowPathStart -> icCall
     *  - slowPathStart -> slowJoinPoint
     * Because the call ICs are fairly long (compared to PICs), we don't reserve the space in each
     * path until the first usage of funGuard (for the in-line path) or slowPathStart (for the
     * out-of-line path).
     */

    /* Initialized only on lowerFunCallOrApply branch. */
    Jump            uncachedCallSlowRejoin;
    CallPatchInfo   uncachedCallPatch;

    {
        MaybeRegisterID origCalleeType, maybeOrigCalleeData;
        RegisterID origCalleeData;

        /* Get the callee in registers. */
        frame.ensureFullRegs(origCallee, &origCalleeType, &maybeOrigCalleeData);
        origCalleeData = maybeOrigCalleeData.reg();
        PinRegAcrossSyncAndKill p1(frame, origCalleeData), p2(frame, origCalleeType);

        if (lowerFunCallOrApply) {
            MaybeRegisterID origThisType, maybeOrigThisData;
            RegisterID origThisData;
            {
                /* Get thisv in registers. */
                frame.ensureFullRegs(origThis, &origThisType, &maybeOrigThisData);
                origThisData = maybeOrigThisData.reg();
                PinRegAcrossSyncAndKill p3(frame, origThisData), p4(frame, origThisType);

                /* Leaves pinned regs untouched. */
                frame.syncAndKill(Uses(argc + 2));
            }

            checkCallApplySpeculation(argc, origCallee, origThis,
                                      origCalleeType, origCalleeData,
                                      origThisType, origThisData,
                                      &uncachedCallSlowRejoin, &uncachedCallPatch);

            icCalleeType = origThisType;
            icCalleeData = origThisData;
            icRvalAddr = frame.addressOf(origThis);

            /*
             * For f.call(), since we compile the ic under the (checked)
             * assumption that call == js_fun_call, we still have a static
             * frame size. For f.apply(), the frame size depends on the dynamic
             * length of the array passed to apply.
             */
            if (*PC == JSOP_FUNCALL)
                callIC.frameSize.initStatic(frame.totalDepth(), argc - 1);
            else
                callIC.frameSize.initDynamic();
        } else {
            /* Leaves pinned regs untouched. */
            frame.syncAndKill(Uses(argc + 2));

            icCalleeType = origCalleeType;
            icCalleeData = origCalleeData;
            icRvalAddr = frame.addressOf(origCallee);
            callIC.frameSize.initStatic(frame.totalDepth(), argc);
        }
    }

    callFrameSize = callIC.frameSize;

    callIC.typeMonitored = monitored(PC) || hasTypeBarriers(PC);

    /* Test the type if necessary. Failing this always takes a really slow path. */
    MaybeJump notObjectJump;
    if (icCalleeType.isSet())
        notObjectJump = masm.testObject(Assembler::NotEqual, icCalleeType.reg());

    /*
     * For an optimized apply, keep icCalleeData in a callee-saved register for
     * the subsequent ic::SplatApplyArgs call.
     */
    Registers tempRegs(Registers::AvailRegs);
    if (callIC.frameSize.isDynamic() && !Registers::isSaved(icCalleeData)) {
        RegisterID x = tempRegs.takeAnyReg(Registers::SavedRegs).reg();
        masm.move(icCalleeData, x);
        icCalleeData = x;
    } else {
        tempRegs.takeReg(icCalleeData);
    }

    /* Reserve space just before initialization of funGuard. */
    RESERVE_IC_SPACE(masm);

    /*
     * Guard on the callee identity. This misses on the first run. If the
     * callee is scripted, compiled/compilable, and argc == nargs, then this
     * guard is patched, and the compiled code address is baked in.
     */
    Jump j = masm.branchPtrWithPatch(Assembler::NotEqual, icCalleeData, callIC.funGuard);
    callIC.funJump = j;

    /* Reserve space just before initialization of slowPathStart. */
    RESERVE_OOL_SPACE(stubcc.masm);

    Jump rejoin1, rejoin2;
    {
        RESERVE_OOL_SPACE(stubcc.masm);
        stubcc.linkExitDirect(j, stubcc.masm.label());
        callIC.slowPathStart = stubcc.masm.label();

        RegisterID tmp = tempRegs.takeAnyReg().reg();

        /*
         * Test if the callee is even a function. If this doesn't match, we
         * take a _really_ slow path later.
         */
        Jump notFunction = stubcc.masm.testFunction(Assembler::NotEqual, icCalleeData, tmp);

        /* Test if the function is scripted. */
        stubcc.masm.load16(Address(icCalleeData, offsetof(JSFunction, flags)), tmp);
        stubcc.masm.and32(Imm32(JSFUN_KINDMASK), tmp);
        Jump isNative = stubcc.masm.branch32(Assembler::Below, tmp, Imm32(JSFUN_INTERPRETED));
        tempRegs.putReg(tmp);

        /*
         * N.B. After this call, the frame will have a dynamic frame size.
         * Check after the function is known not to be a native so that the
         * catch-all/native path has a static depth.
         */
        if (callIC.frameSize.isDynamic())
            OOL_STUBCALL(ic::SplatApplyArgs, REJOIN_CALL_SPLAT);

        /*
         * No-op jump that gets patched by ic::New/Call to the stub generated
         * by generateFullCallStub.
         */
        Jump toPatch = stubcc.masm.jump();
        toPatch.linkTo(stubcc.masm.label(), &stubcc.masm);
        callIC.oolJump = toPatch;
        callIC.icCall = stubcc.masm.label();

        RejoinState rejoinState = callIC.frameSize.rejoinState(PC, false);

        /*
         * At this point the function is definitely scripted, so we try to
         * compile it and patch either funGuard/funJump or oolJump. This code
         * is only executed once.
         */
        callIC.addrLabel1 = stubcc.masm.moveWithPatch(ImmPtr(NULL), Registers::ArgReg1);
        void *icFunPtr = JS_FUNC_TO_DATA_PTR(void *, callingNew ? ic::New : ic::Call);
        if (callIC.frameSize.isStatic()) {
            callIC.oolCall = OOL_STUBCALL_LOCAL_SLOTS(icFunPtr, rejoinState, frame.totalDepth());
        } else {
            callIC.oolCall = OOL_STUBCALL_LOCAL_SLOTS(icFunPtr, rejoinState, -1);
        }

        callIC.funObjReg = icCalleeData;

        /*
         * The IC call either returns NULL, meaning call completed, or a
         * function pointer to jump to.
         */
        rejoin1 = stubcc.masm.branchTestPtr(Assembler::Zero, Registers::ReturnReg,
                                            Registers::ReturnReg);
        if (callIC.frameSize.isStatic())
            stubcc.masm.move(Imm32(callIC.frameSize.staticArgc()), JSParamReg_Argc);
        else
            stubcc.masm.load32(FrameAddress(VMFrame::offsetOfDynamicArgc()), JSParamReg_Argc);
        stubcc.masm.loadPtr(FrameAddress(VMFrame::offsetOfRegsSp()), JSFrameReg);
        callPatch.hasSlowNcode = true;
        callPatch.slowNcodePatch =
            stubcc.masm.storePtrWithPatch(ImmPtr(NULL),
                                          Address(JSFrameReg, StackFrame::offsetOfNcode()));
        stubcc.masm.jump(Registers::ReturnReg);



        /*
         * This ool path is the catch-all for everything but scripted function
         * callees. For native functions, ic::NativeNew/NativeCall will repatch
         * funGaurd/funJump with a fast call stub. All other cases
         * (non-function callable objects and invalid callees) take the slow
         * path through js::Invoke.
         */
        if (notObjectJump.isSet())
            stubcc.linkExitDirect(notObjectJump.get(), stubcc.masm.label());
        notFunction.linkTo(stubcc.masm.label(), &stubcc.masm);
        isNative.linkTo(stubcc.masm.label(), &stubcc.masm);

        callIC.addrLabel2 = stubcc.masm.moveWithPatch(ImmPtr(NULL), Registers::ArgReg1);
        OOL_STUBCALL(callingNew ? ic::NativeNew : ic::NativeCall, rejoinState);

        rejoin2 = stubcc.masm.jump();
    }

    /*
     * If the call site goes to a closure over the same function, it will
     * generate an out-of-line stub that joins back here.
     */
    callIC.hotPathLabel = masm.label();

    uint32_t flags = 0;
    if (callingNew)
        flags |= StackFrame::CONSTRUCTING;

    InlineFrameAssembler inlFrame(masm, callIC, flags);
    callPatch.hasFastNcode = true;
    callPatch.fastNcodePatch = inlFrame.assemble(NULL, PC);

    callIC.hotJump = masm.jump();
    callIC.joinPoint = callPatch.joinPoint = masm.label();
    callIC.callIndex = callSites.length();
    addReturnSite();
    if (lowerFunCallOrApply)
        uncachedCallPatch.joinPoint = callIC.joinPoint;

    /*
     * We've placed hotJump, joinPoint and hotPathLabel, and no other labels are located by offset
     * in the in-line path so we can check the IC space now.
     */
    CHECK_IC_SPACE();

    JSValueType type = knownPushedType(0);

    frame.popn(argc + 2);
    frame.takeReg(JSReturnReg_Type);
    frame.takeReg(JSReturnReg_Data);
    frame.pushRegs(JSReturnReg_Type, JSReturnReg_Data, type);

    BarrierState barrier = testBarrier(JSReturnReg_Type, JSReturnReg_Data,
                                       /* testUndefined = */ false,
                                       /* testReturn = */ true);

    /*
     * Now that the frame state is set, generate the rejoin path. Note that, if
     * lowerFunCallOrApply, we cannot just call 'stubcc.rejoin' since the return
     * value has been placed at vp[1] which is not the stack address associated
     * with frame.peek(-1).
     */
    callIC.slowJoinPoint = stubcc.masm.label();
    rejoin1.linkTo(callIC.slowJoinPoint, &stubcc.masm);
    rejoin2.linkTo(callIC.slowJoinPoint, &stubcc.masm);
    JaegerSpew(JSpew_Insns, " ---- BEGIN SLOW RESTORE CODE ---- \n");
    frame.reloadEntry(stubcc.masm, icRvalAddr, frame.peek(-1));
    stubcc.crossJump(stubcc.masm.jump(), masm.label());
    JaegerSpew(JSpew_Insns, " ---- END SLOW RESTORE CODE ---- \n");

    CHECK_OOL_SPACE();

    if (lowerFunCallOrApply)
        stubcc.crossJump(uncachedCallSlowRejoin, masm.label());

    callICs.append(callIC);
    callPatches.append(callPatch);
    if (lowerFunCallOrApply)
        callPatches.append(uncachedCallPatch);

    finishBarrier(barrier, REJOIN_FALLTHROUGH, 0);
    return true;
#endif
}

/* Maximum number of calls we will inline at the same site. */
static const uint32_t INLINE_SITE_LIMIT = 5;

CompileStatus
mjit::Compiler::inlineScriptedFunction(uint32_t argc, bool callingNew)
{
    JS_ASSERT(inlining());

    /* We already know which frames we are inlining at each PC, so scan the list of inline frames. */
    bool calleeMultipleReturns = false;
    Vector<JSScript *> inlineCallees(CompilerAllocPolicy(cx, *this));
    for (unsigned i = 0; i < ssa.numFrames(); i++) {
        if (ssa.iterFrame(i).parent == a->inlineIndex && ssa.iterFrame(i).parentpc == PC) {
            JSScript *script = ssa.iterFrame(i).script;
            inlineCallees.append(script);
            if (script->analysis()->numReturnSites() > 1)
                calleeMultipleReturns = true;
        }
    }

    if (inlineCallees.empty())
        return Compile_InlineAbort;

    JS_ASSERT(!monitored(PC));

    /*
     * Remove all dead entries from the frame's tracker. We will not recognize
     * them as dead after pushing the new frame.
     */
    frame.pruneDeadEntries();

    RegisterAllocation *exitState = NULL;
    if (inlineCallees.length() > 1 || calleeMultipleReturns) {
        /*
         * Multiple paths through the callees, get a register allocation for
         * the various incoming edges.
         */
        exitState = frame.computeAllocation(PC + JSOP_CALL_LENGTH);
    }

    /*
     * If this is a polymorphic callsite, get a register for the callee too.
     * After this, do not touch the register state in the current frame until
     * stubs for all callees have been generated.
     */
    FrameEntry *origCallee = frame.peek(-((int)argc + 2));
    FrameEntry *entrySnapshot = NULL;
    MaybeRegisterID calleeReg;
    if (inlineCallees.length() > 1) {
        frame.forgetMismatchedObject(origCallee);
        calleeReg = frame.tempRegForData(origCallee);

        entrySnapshot = frame.snapshotState();
        if (!entrySnapshot)
            return Compile_Error;
    }
    MaybeJump calleePrevious;

    JSValueType returnType = knownPushedType(0);

    bool needReturnValue = JSOP_POP != (JSOp)*(PC + JSOP_CALL_LENGTH);
    bool syncReturnValue = needReturnValue && returnType == JSVAL_TYPE_UNKNOWN;

    /* Track register state after the call. */
    bool returnSet = false;
    AnyRegisterID returnRegister;
    const FrameEntry *returnEntry = NULL;

    Vector<Jump, 4, CompilerAllocPolicy> returnJumps(CompilerAllocPolicy(cx, *this));

    for (unsigned i = 0; i < inlineCallees.length(); i++) {
        if (entrySnapshot)
            frame.restoreFromSnapshot(entrySnapshot);
        
        JSScript *script = inlineCallees[i];
        CompileStatus status;

        status = pushActiveFrame(script, argc);
        if (status != Compile_Okay)
            return status;

        a->exitState = exitState;

        JaegerSpew(JSpew_Inlining, "inlining call to script (file \"%s\") (line \"%d\")\n",
                   script->filename, script->lineno);

        if (calleePrevious.isSet()) {
            calleePrevious.get().linkTo(masm.label(), &masm);
            calleePrevious = MaybeJump();
        }

        if (i + 1 != inlineCallees.length()) {
            /* Guard on the callee, except when this object must be the callee. */
            JS_ASSERT(calleeReg.isSet());
            calleePrevious = masm.branchPtr(Assembler::NotEqual, calleeReg.reg(), ImmPtr(script->function()));
        }

        a->returnJumps = &returnJumps;
        a->needReturnValue = needReturnValue;
        a->syncReturnValue = syncReturnValue;
        a->returnValueDouble = returnType == JSVAL_TYPE_DOUBLE;
        if (returnSet) {
            a->returnSet = true;
            a->returnRegister = returnRegister;
        }

        /*
         * Update the argument frame entries in place if the callee has had an
         * argument inferred as double but we are passing an int.
         */
        ensureDoubleArguments();

        markUndefinedLocals();

        status = methodEntryHelper();
        if (status == Compile_Okay)
            status = generateMethod();

        if (status != Compile_Okay) {
            popActiveFrame();
            if (status == Compile_Abort) {
                /* The callee is uncompileable, mark it as uninlineable and retry. */
                script->uninlineable = true;
                types::MarkTypeObjectFlags(cx, script->function(),
                                           types::OBJECT_FLAG_UNINLINEABLE);
                return Compile_Retry;
            }
            return status;
        }

        if (needReturnValue && !returnSet) {
            if (a->returnSet) {
                returnSet = true;
                returnRegister = a->returnRegister;
            } else {
                returnEntry = a->returnEntry;
            }
        }

        popActiveFrame();

        if (i + 1 != inlineCallees.length())
            returnJumps.append(masm.jump());
    }

    for (unsigned i = 0; i < returnJumps.length(); i++)
        returnJumps[i].linkTo(masm.label(), &masm);

    frame.popn(argc + 2);

    if (entrySnapshot)
        cx->array_delete(entrySnapshot);

    if (exitState)
        frame.discardForJoin(exitState, analysis->getCode(PC).stackDepth - (argc + 2));

    if (returnSet) {
        frame.takeReg(returnRegister);
        if (returnRegister.isReg())
            frame.pushTypedPayload(returnType, returnRegister.reg());
        else
            frame.pushDouble(returnRegister.fpreg());
    } else if (returnEntry) {
        frame.pushCopyOf((FrameEntry *) returnEntry);
    } else {
        frame.pushSynced(JSVAL_TYPE_UNKNOWN);
    }

    JaegerSpew(JSpew_Inlining, "finished inlining call to script (file \"%s\") (line \"%d\")\n",
               script->filename, script->lineno);

    return Compile_Okay;
}

/*
 * This function must be called immediately after any instruction which could
 * cause a new StackFrame to be pushed and could lead to a new debug trap
 * being set. This includes any API callbacks and any scripted or native call.
 */
void
mjit::Compiler::addCallSite(const InternalCallSite &site)
{
    callSites.append(site);
}

void
mjit::Compiler::inlineStubCall(void *stub, RejoinState rejoin, Uses uses)
{
    DataLabelPtr inlinePatch;
    Call cl = emitStubCall(stub, &inlinePatch);
    InternalCallSite site(masm.callReturnOffset(cl), a->inlineIndex, PC,
                          rejoin, false);
    site.inlinePatch = inlinePatch;
    if (loop && loop->generatingInvariants()) {
        Jump j = masm.jump();
        Label l = masm.label();
        loop->addInvariantCall(j, l, false, false, callSites.length(), uses);
    }
    addCallSite(site);
}

bool
mjit::Compiler::compareTwoValues(JSContext *cx, JSOp op, const Value &lhs, const Value &rhs)
{
    JS_ASSERT(lhs.isPrimitive());
    JS_ASSERT(rhs.isPrimitive());

    if (lhs.isString() && rhs.isString()) {
        int32_t cmp;
        CompareStrings(cx, lhs.toString(), rhs.toString(), &cmp);
        switch (op) {
          case JSOP_LT:
            return cmp < 0;
          case JSOP_LE:
            return cmp <= 0;
          case JSOP_GT:
            return cmp > 0;
          case JSOP_GE:
            return cmp >= 0;
          case JSOP_EQ:
            return cmp == 0;
          case JSOP_NE:
            return cmp != 0;
          default:
            JS_NOT_REACHED("NYI");
        }
    } else {
        double ld, rd;

        /* These should be infallible w/ primitives. */
        JS_ALWAYS_TRUE(ToNumber(cx, lhs, &ld));
        JS_ALWAYS_TRUE(ToNumber(cx, rhs, &rd));
        switch(op) {
          case JSOP_LT:
            return ld < rd;
          case JSOP_LE:
            return ld <= rd;
          case JSOP_GT:
            return ld > rd;
          case JSOP_GE:
            return ld >= rd;
          case JSOP_EQ: /* fall through */
          case JSOP_NE:
            /* Special case null/undefined/void comparisons. */
            if (lhs.isNullOrUndefined()) {
                if (rhs.isNullOrUndefined())
                    return op == JSOP_EQ;
                return op == JSOP_NE;
            }
            if (rhs.isNullOrUndefined())
                return op == JSOP_NE;

            /* Normal return. */
            return (op == JSOP_EQ) ? (ld == rd) : (ld != rd);
          default:
            JS_NOT_REACHED("NYI");
        }
    }

    JS_NOT_REACHED("NYI");
    return false;
}

bool
mjit::Compiler::constantFoldBranch(jsbytecode *target, bool taken)
{
    if (taken) {
        if (!frame.syncForBranch(target, Uses(0)))
            return false;
        Jump j = masm.jump();
        if (!jumpAndRun(j, target))
            return false;
    } else {
        /*
         * Branch is never taken, but clean up any loop
         * if this is a backedge.
         */
        if (target < PC && !finishLoop(target))
            return false;
    }
    return true;
}

bool
mjit::Compiler::emitStubCmpOp(BoolStub stub, jsbytecode *target, JSOp fused)
{
    if (target)
        frame.syncAndKillEverything();
    else
        frame.syncAndKill(Uses(2));

    prepareStubCall(Uses(2));
    INLINE_STUBCALL(stub, target ? REJOIN_BRANCH : REJOIN_PUSH_BOOLEAN);
    frame.popn(2);

    if (!target) {
        frame.takeReg(Registers::ReturnReg);
        frame.pushTypedPayload(JSVAL_TYPE_BOOLEAN, Registers::ReturnReg);
        return true;
    }

    JS_ASSERT(fused == JSOP_IFEQ || fused == JSOP_IFNE);
    Jump j = masm.branchTest32(GetStubCompareCondition(fused), Registers::ReturnReg,
                               Registers::ReturnReg);
    return jumpAndRun(j, target);
}

void
mjit::Compiler::jsop_setprop_slow(PropertyName *name)
{
    prepareStubCall(Uses(2));
    masm.move(ImmPtr(name), Registers::ArgReg1);
    INLINE_STUBCALL(STRICT_VARIANT(stubs::SetName), REJOIN_FALLTHROUGH);
    JS_STATIC_ASSERT(JSOP_SETNAME_LENGTH == JSOP_SETPROP_LENGTH);
    frame.shimmy(1);
    if (script->hasScriptCounts)
        bumpPropCount(PC, PCCounts::PROP_OTHER);
}

void
mjit::Compiler::jsop_getprop_slow(PropertyName *name, bool forPrototype)
{
    /* See ::jsop_getprop */
    RejoinState rejoin = forPrototype ? REJOIN_THIS_PROTOTYPE : REJOIN_GETTER;

    prepareStubCall(Uses(1));
    masm.move(ImmPtr(name), Registers::ArgReg1);
    INLINE_STUBCALL(forPrototype ? stubs::GetPropNoCache : stubs::GetProp, rejoin);

    if (!forPrototype)
        testPushedType(rejoin, -1, /* ool = */ false);

    frame.pop();
    frame.pushSynced(JSVAL_TYPE_UNKNOWN);

    if (script->hasScriptCounts)
        bumpPropCount(PC, PCCounts::PROP_OTHER);
}

#ifdef JS_MONOIC
void
mjit::Compiler::passMICAddress(GlobalNameICInfo &ic)
{
    ic.addrLabel = stubcc.masm.moveWithPatch(ImmPtr(NULL), Registers::ArgReg1);
}
#endif

#if defined JS_POLYIC
void
mjit::Compiler::passICAddress(BaseICInfo *ic)
{
    ic->paramAddr = stubcc.masm.moveWithPatch(ImmPtr(NULL), Registers::ArgReg1);
}

bool
mjit::Compiler::jsop_getprop(PropertyName *name, JSValueType knownType,
                             bool doTypeCheck, bool forPrototype)
{
    FrameEntry *top = frame.peek(-1);

    /*
     * Use a different rejoin for GETPROP computing the 'this' object, as we
     * can't use the current bytecode within InternalInterpret to tell this is
     * fetching the 'this' value.
     */
    RejoinState rejoin = REJOIN_GETTER;
    if (forPrototype) {
        JS_ASSERT(top->isType(JSVAL_TYPE_OBJECT) &&
                  name == cx->runtime->atomState.classPrototypeAtom);
        rejoin = REJOIN_THIS_PROTOTYPE;
    }

    /* Handle length accesses on known strings without using a PIC. */
    if (name == cx->runtime->atomState.lengthAtom &&
        top->isType(JSVAL_TYPE_STRING) &&
        (!cx->typeInferenceEnabled() || knownPushedType(0) == JSVAL_TYPE_INT32)) {
        if (top->isConstant()) {
            JSString *str = top->getValue().toString();
            Value v;
            v.setNumber(uint32_t(str->length()));
            frame.pop();
            frame.push(v);
        } else {
            RegisterID str = frame.ownRegForData(top);
            masm.loadPtr(Address(str, JSString::offsetOfLengthAndFlags()), str);
            masm.urshift32(Imm32(JSString::LENGTH_SHIFT), str);
            frame.pop();
            frame.pushTypedPayload(JSVAL_TYPE_INT32, str);
        }
        return true;
    }

    if (top->mightBeType(JSVAL_TYPE_OBJECT) &&
        JSOp(*PC) == JSOP_LENGTH && cx->typeInferenceEnabled() &&
        !hasTypeBarriers(PC) && knownPushedType(0) == JSVAL_TYPE_INT32) {
        /* Check if this is an array we can make a loop invariant entry for. */
        if (loop && loop->generatingInvariants()) {
            CrossSSAValue topv(a->inlineIndex, analysis->poppedValue(PC, 0));
            FrameEntry *fe = loop->invariantLength(topv);
            if (fe) {
                frame.learnType(fe, JSVAL_TYPE_INT32, false);
                frame.pop();
                frame.pushCopyOf(fe);
                if (script->hasScriptCounts)
                    bumpPropCount(PC, PCCounts::PROP_STATIC);
                return true;
            }
        }

        types::TypeSet *types = analysis->poppedTypes(PC, 0);

        /*
         * Check if we are accessing the 'length' property of a known dense array.
         * Note that if the types are known to indicate dense arrays, their lengths
         * must fit in an int32.
         */
        if (!types->hasObjectFlags(cx, types::OBJECT_FLAG_NON_DENSE_ARRAY)) {
            bool isObject = top->isTypeKnown();
            if (!isObject) {
                Jump notObject = frame.testObject(Assembler::NotEqual, top);
                stubcc.linkExit(notObject, Uses(1));
                stubcc.leave();
                stubcc.masm.move(ImmPtr(name), Registers::ArgReg1);
                OOL_STUBCALL(stubs::GetProp, rejoin);
                if (rejoin == REJOIN_GETTER)
                    testPushedType(rejoin, -1);
            }
            RegisterID result = frame.allocReg();
            RegisterID reg = frame.tempRegForData(top);
            frame.pop();
            masm.loadPtr(Address(reg, JSObject::offsetOfElements()), result);
            masm.load32(Address(result, ObjectElements::offsetOfLength()), result);
            frame.pushTypedPayload(JSVAL_TYPE_INT32, result);
            if (script->hasScriptCounts)
                bumpPropCount(PC, PCCounts::PROP_DEFINITE);
            if (!isObject)
                stubcc.rejoin(Changes(1));
            return true;
        }

        /*
         * Check if we're accessing the 'length' property of a typed array.
         * The typed array length always fits in an int32.
         */
        if (!types->hasObjectFlags(cx, types::OBJECT_FLAG_NON_TYPED_ARRAY)) {
            if (top->isConstant()) {
                JSObject *obj = &top->getValue().toObject();
                uint32_t length = TypedArray::length(obj);
                frame.pop();
                frame.push(Int32Value(length));
                return true;
            }
            bool isObject = top->isTypeKnown();
            if (!isObject) {
                Jump notObject = frame.testObject(Assembler::NotEqual, top);
                stubcc.linkExit(notObject, Uses(1));
                stubcc.leave();
                stubcc.masm.move(ImmPtr(name), Registers::ArgReg1);
                OOL_STUBCALL(stubs::GetProp, rejoin);
                if (rejoin == REJOIN_GETTER)
                    testPushedType(rejoin, -1);
            }
            RegisterID reg = frame.copyDataIntoReg(top);
            frame.pop();
            masm.loadPayload(Address(reg, TypedArray::lengthOffset()), reg);
            frame.pushTypedPayload(JSVAL_TYPE_INT32, reg);
            if (script->hasScriptCounts)
                bumpPropCount(PC, PCCounts::PROP_DEFINITE);
            if (!isObject)
                stubcc.rejoin(Changes(1));
            return true;
        }

        /*
         * Check if we are accessing the 'length' of the lazy arguments for the
         * current frame.
         */
        if (types->isMagicArguments(cx)) {
            frame.pop();
            frame.pushWord(Address(JSFrameReg, StackFrame::offsetOfNumActual()), JSVAL_TYPE_INT32);
            if (script->hasScriptCounts)
                bumpPropCount(PC, PCCounts::PROP_DEFINITE);
            return true;
        }
    }

    /* If the access will definitely be fetching a particular value, nop it. */
    bool testObject;
    JSObject *singleton =
        (*PC == JSOP_GETPROP || *PC == JSOP_CALLPROP) ? pushedSingleton(0) : NULL;
    if (singleton && singleton->isFunction() && !hasTypeBarriers(PC)) {
        Rooted<jsid> id(cx, NameToId(name));
        if (testSingletonPropertyTypes(top, id, &testObject)) {
            if (testObject) {
                Jump notObject = frame.testObject(Assembler::NotEqual, top);
                stubcc.linkExit(notObject, Uses(1));
                stubcc.leave();
                stubcc.masm.move(ImmPtr(name), Registers::ArgReg1);
                OOL_STUBCALL(stubs::GetProp, REJOIN_FALLTHROUGH);
                testPushedType(REJOIN_FALLTHROUGH, -1);
            }

            frame.pop();
            frame.push(ObjectValue(*singleton));

            if (script->hasScriptCounts && cx->typeInferenceEnabled())
                bumpPropCount(PC, PCCounts::PROP_STATIC);

            if (testObject)
                stubcc.rejoin(Changes(1));

            return true;
        }
    }

    /* Check if this is a property access we can make a loop invariant entry for. */
    if (loop && loop->generatingInvariants() && !hasTypeBarriers(PC)) {
        CrossSSAValue topv(a->inlineIndex, analysis->poppedValue(PC, 0));
        if (FrameEntry *fe = loop->invariantProperty(topv, NameToId(name))) {
            if (knownType != JSVAL_TYPE_UNKNOWN && knownType != JSVAL_TYPE_DOUBLE)
                frame.learnType(fe, knownType, false);
            frame.pop();
            frame.pushCopyOf(fe);
            if (script->hasScriptCounts)
                bumpPropCount(PC, PCCounts::PROP_STATIC);
            return true;
        }
    }

    /* If the incoming type will never PIC, take slow path. */
    if (top->isNotType(JSVAL_TYPE_OBJECT)) {
        jsop_getprop_slow(name, forPrototype);
        return true;
    }

    frame.forgetMismatchedObject(top);

    /*
     * Check if we are accessing a known type which always has the property
     * in a particular inline slot. Get the property directly in this case,
     * without using an IC.
     */
    jsid id = NameToId(name);
    types::TypeSet *types = frame.extra(top).types;
    if (types && !types->unknownObject() &&
        types->getObjectCount() == 1 &&
        types->getTypeObject(0) != NULL &&
        !types->getTypeObject(0)->unknownProperties() &&
        id == types::MakeTypeId(cx, id)) {
        JS_ASSERT(!forPrototype);
        types::TypeObject *object = types->getTypeObject(0);
        types::TypeSet *propertyTypes = object->getProperty(cx, id, false);
        if (!propertyTypes)
            return false;
        if (propertyTypes->isDefiniteProperty() &&
            !propertyTypes->isOwnProperty(cx, object, true)) {
            types->addFreeze(cx);
            uint32_t slot = propertyTypes->definiteSlot();
            bool isObject = top->isTypeKnown();
            if (!isObject) {
                Jump notObject = frame.testObject(Assembler::NotEqual, top);
                stubcc.linkExit(notObject, Uses(1));
                stubcc.leave();
                stubcc.masm.move(ImmPtr(name), Registers::ArgReg1);
                OOL_STUBCALL(stubs::GetProp, rejoin);
                if (rejoin == REJOIN_GETTER)
                    testPushedType(rejoin, -1);
            }
            RegisterID reg = frame.tempRegForData(top);
            frame.pop();

            if (script->hasScriptCounts)
                bumpPropCount(PC, PCCounts::PROP_DEFINITE);

            Address address(reg, JSObject::getFixedSlotOffset(slot));
            BarrierState barrier = pushAddressMaybeBarrier(address, knownType, false);
            if (!isObject)
                stubcc.rejoin(Changes(1));
            finishBarrier(barrier, rejoin, 0);

            return true;
        }
    }

    /* Check for a dynamic dispatch. */
    if (cx->typeInferenceEnabled()) {
        if (*PC == JSOP_CALLPROP && jsop_getprop_dispatch(name))
            return true;
    }

    if (script->hasScriptCounts)
        bumpPropCount(PC, PCCounts::PROP_OTHER);

    /*
     * These two must be loaded first. The objReg because the string path
     * wants to read it, and the shapeReg because it could cause a spill that
     * the string path wouldn't sink back.
     */
    RegisterID objReg = frame.copyDataIntoReg(top);
    RegisterID shapeReg = frame.allocReg();

    RESERVE_IC_SPACE(masm);

    PICGenInfo pic(ic::PICInfo::GET, JSOp(*PC));

    /*
     * If this access has been on a shape with a getter hook, make preparations
     * so that we can generate a stub to call the hook directly (rather than be
     * forced to make a stub call). Sync the stack up front and kill all
     * registers so that PIC stubs can contain calls, and always generate a
     * type barrier if inference is enabled (known property types do not
     * reflect properties with getter hooks).
     */
    pic.canCallHook = pic.forcedTypeBarrier =
        !forPrototype &&
        JSOp(*PC) == JSOP_GETPROP &&
        analysis->getCode(PC).accessGetter;

    /* Guard that the type is an object. */
    Label typeCheck;
    if (doTypeCheck && !top->isTypeKnown()) {
        RegisterID reg = frame.tempRegForType(top);
        pic.typeReg = reg;

        if (pic.canCallHook) {
            PinRegAcrossSyncAndKill p1(frame, reg);
            frame.syncAndKillEverything();
        }

        /* Start the hot path where it's easy to patch it. */
        pic.fastPathStart = masm.label();
        Jump j = masm.testObject(Assembler::NotEqual, reg);
        typeCheck = masm.label();
        RETURN_IF_OOM(false);

        pic.typeCheck = stubcc.linkExit(j, Uses(1));
        pic.hasTypeCheck = true;
    } else {
        if (pic.canCallHook)
            frame.syncAndKillEverything();

        pic.fastPathStart = masm.label();
        pic.hasTypeCheck = false;
        pic.typeReg = Registers::ReturnReg;
    }

    pic.shapeReg = shapeReg;
    pic.name = name;

    /* Guard on shape. */
    masm.loadShape(objReg, shapeReg);
    pic.shapeGuard = masm.label();

    DataLabelPtr inlineShapeLabel;
    Jump j = masm.branchPtrWithPatch(Assembler::NotEqual, shapeReg,
                                     inlineShapeLabel, ImmPtr(NULL));
    Label inlineShapeJump = masm.label();

    RESERVE_OOL_SPACE(stubcc.masm);
    pic.slowPathStart = stubcc.linkExit(j, Uses(1));
    pic.cached = !forPrototype;

    stubcc.leave();
    passICAddress(&pic);
    pic.slowPathCall = OOL_STUBCALL(ic::GetProp, rejoin);
    CHECK_OOL_SPACE();
    if (rejoin == REJOIN_GETTER)
        testPushedType(rejoin, -1);

    /* Load the base slot address. */
    Label dslotsLoadLabel = masm.loadPtrWithPatchToLEA(Address(objReg, JSObject::offsetOfSlots()),
                                                               objReg);

    /* Copy the slot value to the expression stack. */
    Address slot(objReg, 1 << 24);
    frame.pop();

    Label fastValueLoad = masm.loadValueWithAddressOffsetPatch(slot, shapeReg, objReg);
    pic.fastPathRejoin = masm.label();

    RETURN_IF_OOM(false);

    /* Initialize op labels. */
    GetPropLabels &labels = pic.getPropLabels();
    labels.setDslotsLoad(masm, pic.fastPathRejoin, dslotsLoadLabel);
    labels.setInlineShapeData(masm, pic.shapeGuard, inlineShapeLabel);

    labels.setValueLoad(masm, pic.fastPathRejoin, fastValueLoad);
    if (pic.hasTypeCheck)
        labels.setInlineTypeJump(masm, pic.fastPathStart, typeCheck);
    labels.setInlineShapeJump(masm, pic.shapeGuard, inlineShapeJump);

    CHECK_IC_SPACE();

    pic.objReg = objReg;
    frame.pushRegs(shapeReg, objReg, knownType);
    BarrierState barrier = testBarrier(pic.shapeReg, pic.objReg, false, false,
                                       /* force = */ pic.canCallHook);

    stubcc.rejoin(Changes(1));
    pics.append(pic);

    finishBarrier(barrier, rejoin, 0);
    return true;
}

bool
mjit::Compiler::testSingletonProperty(HandleObject obj, HandleId id)
{
    /*
     * We would like to completely no-op property/global accesses which can
     * produce only a particular JSObject or undefined, provided we can
     * determine the pushed value must not be undefined (or, if it could be
     * undefined, a recompilation will be triggered).
     *
     * If the access definitely goes through obj, either directly or on the
     * prototype chain, then if obj has a defined property now, and the
     * property has a default or method shape, the only way it can produce
     * undefined in the future is if it is deleted. Deletion causes type
     * properties to be explicitly marked with undefined.
     */

    JSObject *nobj = obj;
    while (nobj) {
        if (!nobj->isNative())
            return false;
        if (nobj->getClass()->ops.lookupGeneric)
            return false;
        nobj = nobj->getProto();
    }

    RootedObject holder(cx);
    RootedShape shape(cx);
    if (!obj->lookupGeneric(cx, id, &holder, &shape))
        return false;
    if (!shape)
        return false;

    if (shape->hasDefaultGetter()) {
        if (!shape->hasSlot())
            return false;
        if (holder->getSlot(shape->slot()).isUndefined())
            return false;
    } else {
        return false;
    }

    return true;
}

bool
mjit::Compiler::testSingletonPropertyTypes(FrameEntry *top, HandleId id, bool *testObject)
{
    *testObject = false;

    types::TypeSet *types = frame.extra(top).types;
    if (!types || types->unknownObject())
        return false;

    RootedObject singleton(cx, types->getSingleton(cx));
    if (singleton)
        return testSingletonProperty(singleton, id);

    if (!globalObj)
        return false;

    JSProtoKey key;
    JSValueType type = types->getKnownTypeTag(cx);
    switch (type) {
      case JSVAL_TYPE_STRING:
        key = JSProto_String;
        break;

      case JSVAL_TYPE_INT32:
      case JSVAL_TYPE_DOUBLE:
        key = JSProto_Number;
        break;

      case JSVAL_TYPE_BOOLEAN:
        key = JSProto_Boolean;
        break;

      case JSVAL_TYPE_OBJECT:
      case JSVAL_TYPE_UNKNOWN:
        if (types->getObjectCount() == 1 && !top->isNotType(JSVAL_TYPE_OBJECT)) {
            JS_ASSERT_IF(top->isTypeKnown(), top->isType(JSVAL_TYPE_OBJECT));
            types::TypeObject *object = types->getTypeObject(0);
            if (object && object->proto) {
                Rooted<JSObject*> proto(cx, object->proto);
                if (!testSingletonProperty(proto, id))
                    return false;
                types->addFreeze(cx);

                /* If we don't know this is an object, we will need a test. */
                *testObject = (type != JSVAL_TYPE_OBJECT) && !top->isTypeKnown();
                return true;
            }
        }
        return false;

      default:
        return false;
    }

    RootedObject proto(cx);
    if (!js_GetClassPrototype(cx, globalObj, key, &proto, NULL))
        return false;

    return testSingletonProperty(proto, id);
}

bool
mjit::Compiler::jsop_getprop_dispatch(PropertyName *name)
{
    /*
     * Check for a CALLPROP which is a dynamic dispatch: every value it can
     * push is a singleton, and the pushed value is determined by the type of
     * the object being accessed. Return true if the CALLPROP has been fully
     * processed, false if no code was generated.
     */
    FrameEntry *top = frame.peek(-1);
    if (top->isNotType(JSVAL_TYPE_OBJECT))
        return false;

    RootedId id(cx, NameToId(name));
    if (id.get() != types::MakeTypeId(cx, id))
        return false;

    types::TypeSet *pushedTypes = pushedTypeSet(0);
    if (pushedTypes->unknownObject() || pushedTypes->baseFlags() != 0)
        return false;

    /* Check every pushed value is a singleton. */
    for (unsigned i = 0; i < pushedTypes->getObjectCount(); i++) {
        if (pushedTypes->getTypeObject(i) != NULL)
            return false;
    }

    types::TypeSet *objTypes = analysis->poppedTypes(PC, 0);
    if (objTypes->unknownObject() || objTypes->getObjectCount() == 0)
        return false;

    pushedTypes->addFreeze(cx);

    /* Map each type in the object to the resulting pushed value. */
    Vector<JSObject *> results(CompilerAllocPolicy(cx, *this));

    /*
     * For each type of the base object, check it has no 'own' property for the
     * accessed id and that its prototype does have such a property.
     */
    uint32_t last = 0;
    for (unsigned i = 0; i < objTypes->getObjectCount(); i++) {
        if (objTypes->getSingleObject(i) != NULL)
            return false;
        types::TypeObject *object = objTypes->getTypeObject(i);
        if (!object) {
            results.append((JSObject *) NULL);
            continue;
        }
        if (object->unknownProperties() || !object->proto)
            return false;
        types::TypeSet *ownTypes = object->getProperty(cx, id, false);
        if (ownTypes->isOwnProperty(cx, object, false))
            return false;

        Rooted<JSObject*> proto(cx, object->proto);
        if (!testSingletonProperty(proto, id))
            return false;

        if (proto->getType(cx)->unknownProperties())
            return false;
        types::TypeSet *protoTypes = proto->type()->getProperty(cx, id, false);
        if (!protoTypes)
            return false;
        JSObject *singleton = protoTypes->getSingleton(cx);
        if (!singleton)
            return false;

        results.append(singleton);
        last = i;
    }

    if (oomInVector)
        return false;

    objTypes->addFreeze(cx);

    /* Done filtering, now generate code which dispatches on the type. */

    frame.forgetMismatchedObject(top);

    if (!top->isType(JSVAL_TYPE_OBJECT)) {
        Jump notObject = frame.testObject(Assembler::NotEqual, top);
        stubcc.linkExit(notObject, Uses(1));
    }

    RegisterID reg = frame.tempRegForData(top);
    frame.pinReg(reg);
    RegisterID pushreg = frame.allocReg();
    frame.unpinReg(reg);

    Address typeAddress(reg, JSObject::offsetOfType());

    Vector<Jump> rejoins(CompilerAllocPolicy(cx, *this));
    MaybeJump lastMiss;

    for (unsigned i = 0; i < objTypes->getObjectCount(); i++) {
        types::TypeObject *object = objTypes->getTypeObject(i);
        if (!object) {
            JS_ASSERT(results[i] == NULL);
            continue;
        }
        if (lastMiss.isSet())
            lastMiss.get().linkTo(masm.label(), &masm);

        /*
         * Check that the pushed result is actually in the known pushed types
         * for the bytecode; this bytecode may have type barriers. Redirect to
         * the stub to update said pushed types.
         */
        if (!pushedTypes->hasType(types::Type::ObjectType(results[i]))) {
            JS_ASSERT(hasTypeBarriers(PC));
            if (i == last) {
                stubcc.linkExit(masm.jump(), Uses(1));
                break;
            } else {
                lastMiss.setJump(masm.branchPtr(Assembler::NotEqual, typeAddress, ImmPtr(object)));
                stubcc.linkExit(masm.jump(), Uses(1));
                continue;
            }
        }

        if (i == last) {
            masm.move(ImmPtr(results[i]), pushreg);
            break;
        } else {
            lastMiss.setJump(masm.branchPtr(Assembler::NotEqual, typeAddress, ImmPtr(object)));
            masm.move(ImmPtr(results[i]), pushreg);
            rejoins.append(masm.jump());
        }
    }

    for (unsigned i = 0; i < rejoins.length(); i++)
        rejoins[i].linkTo(masm.label(), &masm);

    stubcc.leave();
    stubcc.masm.move(ImmPtr(name), Registers::ArgReg1);
    OOL_STUBCALL(stubs::GetProp, REJOIN_FALLTHROUGH);
    testPushedType(REJOIN_FALLTHROUGH, -1);

    frame.pop();
    frame.pushTypedPayload(JSVAL_TYPE_OBJECT, pushreg);

    if (script->hasScriptCounts)
        bumpPropCount(PC, PCCounts::PROP_DEFINITE);

    stubcc.rejoin(Changes(2));
    return true;
}

bool
mjit::Compiler::jsop_setprop(PropertyName *name, bool popGuaranteed)
{
    FrameEntry *lhs = frame.peek(-2);
    FrameEntry *rhs = frame.peek(-1);

    /* If the incoming type will never PIC, take slow path. */
    if (lhs->isTypeKnown() && lhs->getKnownType() != JSVAL_TYPE_OBJECT) {
        jsop_setprop_slow(name);
        return true;
    }

    /*
     * If this is a SETNAME to a variable of a non-reentrant outer function,
     * set the variable's slot directly for the active call object.
     */
    if (cx->typeInferenceEnabled() && js_CodeSpec[*PC].format & JOF_NAME) {
        ScriptAnalysis::NameAccess access =
            analysis->resolveNameAccess(cx, NameToId(name), true);
        if (access.nesting) {
            /* Use a SavedReg so it isn't clobbered by the stub call. */
            RegisterID nameReg = frame.allocReg(Registers::SavedRegs).reg();
            Address address = frame.loadNameAddress(access, nameReg);

#ifdef JSGC_INCREMENTAL_MJ
            /* Write barrier. */
            if (cx->compartment->needsBarrier()) {
                stubcc.linkExit(masm.jump(), Uses(0));
                stubcc.leave();

                /* sync() may have overwritten nameReg, so we reload its data. */
                JS_ASSERT(address.base == nameReg);
                stubcc.masm.move(ImmPtr(access.basePointer()), nameReg);
                stubcc.masm.loadPtr(Address(nameReg), nameReg);
                stubcc.masm.addPtr(Imm32(address.offset), nameReg, Registers::ArgReg1);

                OOL_STUBCALL(stubs::WriteBarrier, REJOIN_NONE);
                stubcc.rejoin(Changes(0));
            }
#endif

            frame.storeTo(rhs, address, popGuaranteed);
            frame.shimmy(1);
            frame.freeReg(address.base);
            return true;
        }
    }

    /*
     * Set the property directly if we are accessing a known object which
     * always has the property in a particular inline slot.
     */
    jsid id = NameToId(name);
    types::TypeSet *types = frame.extra(lhs).types;
    if (JSOp(*PC) == JSOP_SETPROP && id == types::MakeTypeId(cx, id) &&
        types && !types->unknownObject() &&
        types->getObjectCount() == 1 &&
        types->getTypeObject(0) != NULL &&
        !types->getTypeObject(0)->unknownProperties()) {
        types::TypeObject *object = types->getTypeObject(0);
        types::TypeSet *propertyTypes = object->getProperty(cx, id, false);
        if (!propertyTypes)
            return false;
        if (propertyTypes->isDefiniteProperty() &&
            !propertyTypes->isOwnProperty(cx, object, true)) {
            types->addFreeze(cx);
            uint32_t slot = propertyTypes->definiteSlot();
            RegisterID reg = frame.tempRegForData(lhs);
            frame.pinReg(reg);
            bool isObject = lhs->isTypeKnown();
            MaybeJump notObject;
            if (!isObject)
                notObject = frame.testObject(Assembler::NotEqual, lhs);
#ifdef JSGC_INCREMENTAL_MJ
            if (cx->compartment->needsBarrier() && propertyTypes->needsBarrier(cx)) {
                /* Write barrier. */
                Jump j = masm.testGCThing(Address(reg, JSObject::getFixedSlotOffset(slot)));
                stubcc.linkExit(j, Uses(0));
                stubcc.leave();
                stubcc.masm.addPtr(Imm32(JSObject::getFixedSlotOffset(slot)),
                                   reg, Registers::ArgReg1);
                OOL_STUBCALL(stubs::GCThingWriteBarrier, REJOIN_NONE);
                stubcc.rejoin(Changes(0));
            }
#endif
            if (!isObject) {
                stubcc.linkExit(notObject.get(), Uses(2));
                stubcc.leave();
                stubcc.masm.move(ImmPtr(name), Registers::ArgReg1);
                OOL_STUBCALL(STRICT_VARIANT(stubs::SetName), REJOIN_FALLTHROUGH);
            }
            frame.storeTo(rhs, Address(reg, JSObject::getFixedSlotOffset(slot)), popGuaranteed);
            frame.unpinReg(reg);
            frame.shimmy(1);
            if (!isObject)
                stubcc.rejoin(Changes(1));
            if (script->hasScriptCounts)
                bumpPropCount(PC, PCCounts::PROP_DEFINITE);
            return true;
        }
    }

    if (script->hasScriptCounts)
        bumpPropCount(PC, PCCounts::PROP_OTHER);

    JSOp op = JSOp(*PC);

#ifdef JSGC_INCREMENTAL_MJ
    /* Write barrier. We don't have type information for JSOP_SETNAME. */
    if (cx->compartment->needsBarrier() &&
        (!types || op == JSOP_SETNAME || types->propertyNeedsBarrier(cx, id)))
    {
        jsop_setprop_slow(name);
        return true;
    }
#endif

    PICGenInfo pic(ic::PICInfo::SET, op);
    pic.name = name;

    if (monitored(PC)) {
        pic.typeMonitored = true;
        types::TypeSet *types = frame.extra(rhs).types;
        if (!types) {
            /* Handle FORNAME and other compound opcodes. Yuck. */
            types = types::TypeSet::make(cx, "unknownRHS");
            if (!types)
                return false;
            types->addType(cx, types::Type::UnknownType());
        }
        pic.rhsTypes = types;
    } else {
        pic.typeMonitored = false;
        pic.rhsTypes = NULL;
    }

    RESERVE_IC_SPACE(masm);
    RESERVE_OOL_SPACE(stubcc.masm);

    /* Guard that the type is an object. */
    Jump typeCheck;
    if (!lhs->isTypeKnown()) {
        RegisterID reg = frame.tempRegForType(lhs);
        pic.typeReg = reg;

        /* Start the hot path where it's easy to patch it. */
        pic.fastPathStart = masm.label();
        Jump j = masm.testObject(Assembler::NotEqual, reg);

        pic.typeCheck = stubcc.linkExit(j, Uses(2));
        stubcc.leave();

        stubcc.masm.move(ImmPtr(name), Registers::ArgReg1);
        OOL_STUBCALL(STRICT_VARIANT(stubs::SetName), REJOIN_FALLTHROUGH);

        typeCheck = stubcc.masm.jump();
        pic.hasTypeCheck = true;
    } else {
        pic.fastPathStart = masm.label();
        pic.hasTypeCheck = false;
        pic.typeReg = Registers::ReturnReg;
    }

    frame.forgetMismatchedObject(lhs);

    /* Get the object into a mutable register. */
    RegisterID objReg = frame.copyDataIntoReg(lhs);
    pic.objReg = objReg;

    /* Get info about the RHS and pin it. */
    ValueRemat vr;
    frame.pinEntry(rhs, vr);
    pic.vr = vr;

    RegisterID shapeReg = frame.allocReg();
    pic.shapeReg = shapeReg;

    frame.unpinEntry(vr);

    /* Guard on shape. */
    masm.loadShape(objReg, shapeReg);
    pic.shapeGuard = masm.label();
    DataLabelPtr inlineShapeData;
    Jump j = masm.branchPtrWithPatch(Assembler::NotEqual, shapeReg,
                                     inlineShapeData, ImmPtr(NULL));
    Label afterInlineShapeJump = masm.label();

    /* Slow path. */
    {
        pic.slowPathStart = stubcc.linkExit(j, Uses(2));

        stubcc.leave();
        passICAddress(&pic);
        pic.slowPathCall = OOL_STUBCALL(ic::SetProp, REJOIN_FALLTHROUGH);
        CHECK_OOL_SPACE();
    }

    /* Load dslots. */
    Label dslotsLoadLabel = masm.loadPtrWithPatchToLEA(Address(objReg, JSObject::offsetOfSlots()),
                                                       objReg);

    /* Store RHS into object slot. */
    Address slot(objReg, 1 << 24);
    DataLabel32 inlineValueStore = masm.storeValueWithAddressOffsetPatch(vr, slot);
    pic.fastPathRejoin = masm.label();

    frame.freeReg(objReg);
    frame.freeReg(shapeReg);

    /* "Pop under", taking out object (LHS) and leaving RHS. */
    frame.shimmy(1);

    /* Finish slow path. */
    {
        if (pic.hasTypeCheck)
            typeCheck.linkTo(stubcc.masm.label(), &stubcc.masm);
        stubcc.rejoin(Changes(1));
    }

    RETURN_IF_OOM(false);

    SetPropLabels &labels = pic.setPropLabels();
    labels.setInlineShapeData(masm, pic.shapeGuard, inlineShapeData);
    labels.setDslotsLoad(masm, pic.fastPathRejoin, dslotsLoadLabel);
    labels.setInlineValueStore(masm, pic.fastPathRejoin, inlineValueStore);
    labels.setInlineShapeJump(masm, pic.shapeGuard, afterInlineShapeJump);

    pics.append(pic);
    return true;
}

void
mjit::Compiler::jsop_name(PropertyName *name, JSValueType type)
{
    /*
     * If this is a NAME for a variable of a non-reentrant outer function, get
     * the variable's slot directly for the active call object. We always need
     * to check for undefined, however.
     */
    if (cx->typeInferenceEnabled()) {
        ScriptAnalysis::NameAccess access =
            analysis->resolveNameAccess(cx, NameToId(name), true);
        if (access.nesting) {
            Address address = frame.loadNameAddress(access);
            JSValueType type = knownPushedType(0);
            BarrierState barrier = pushAddressMaybeBarrier(address, type, true,
                                                           /* testUndefined = */ true);
            finishBarrier(barrier, REJOIN_GETTER, 0);
            return;
        }
    }

    PICGenInfo pic(ic::PICInfo::NAME, JSOp(*PC));

    RESERVE_IC_SPACE(masm);

    pic.shapeReg = frame.allocReg();
    pic.objReg = frame.allocReg();
    pic.typeReg = Registers::ReturnReg;
    pic.name = name;
    pic.hasTypeCheck = false;
    pic.fastPathStart = masm.label();

    /* There is no inline implementation, so we always jump to the slow path or to a stub. */
    pic.shapeGuard = masm.label();
    Jump inlineJump = masm.jump();
    {
        RESERVE_OOL_SPACE(stubcc.masm);
        pic.slowPathStart = stubcc.linkExit(inlineJump, Uses(0));
        stubcc.leave();
        passICAddress(&pic);
        pic.slowPathCall = OOL_STUBCALL(ic::Name, REJOIN_GETTER);
        CHECK_OOL_SPACE();
        testPushedType(REJOIN_GETTER, 0);
    }
    pic.fastPathRejoin = masm.label();

    /* Initialize op labels. */
    ScopeNameLabels &labels = pic.scopeNameLabels();
    labels.setInlineJump(masm, pic.fastPathStart, inlineJump);

    CHECK_IC_SPACE();

    /*
     * We can't optimize away the PIC for the NAME access itself, but if we've
     * only seen a single value pushed by this access, mark it as such and
     * recompile if a different value becomes possible.
     */
    JSObject *singleton = pushedSingleton(0);
    if (singleton) {
        frame.push(ObjectValue(*singleton));
        frame.freeReg(pic.shapeReg);
        frame.freeReg(pic.objReg);
    } else {
        frame.pushRegs(pic.shapeReg, pic.objReg, type);
    }
    BarrierState barrier = testBarrier(pic.shapeReg, pic.objReg, /* testUndefined = */ true);

    stubcc.rejoin(Changes(1));

    pics.append(pic);

    finishBarrier(barrier, REJOIN_GETTER, 0);
}

bool
mjit::Compiler::jsop_xname(PropertyName *name)
{
    /*
     * If this is a GETXPROP for a variable of a non-reentrant outer function,
     * treat in the same way as a NAME.
     */
    if (cx->typeInferenceEnabled()) {
        ScriptAnalysis::NameAccess access =
            analysis->resolveNameAccess(cx, NameToId(name), true);
        if (access.nesting) {
            frame.pop();
            Address address = frame.loadNameAddress(access);
            JSValueType type = knownPushedType(0);
            BarrierState barrier = pushAddressMaybeBarrier(address, type, true,
                                                           /* testUndefined = */ true);
            finishBarrier(barrier, REJOIN_GETTER, 0);
            return true;
        }
    }

    PICGenInfo pic(ic::PICInfo::XNAME, JSOp(*PC));

    FrameEntry *fe = frame.peek(-1);
    if (fe->isNotType(JSVAL_TYPE_OBJECT)) {
        return jsop_getprop(name, knownPushedType(0));
    }

    if (!fe->isTypeKnown()) {
        Jump notObject = frame.testObject(Assembler::NotEqual, fe);
        stubcc.linkExit(notObject, Uses(1));
    }

    frame.forgetMismatchedObject(fe);

    RESERVE_IC_SPACE(masm);

    pic.shapeReg = frame.allocReg();
    pic.objReg = frame.copyDataIntoReg(fe);
    pic.typeReg = Registers::ReturnReg;
    pic.name = name;
    pic.hasTypeCheck = false;
    pic.fastPathStart = masm.label();

    /* There is no inline implementation, so we always jump to the slow path or to a stub. */
    pic.shapeGuard = masm.label();
    Jump inlineJump = masm.jump();
    {
        RESERVE_OOL_SPACE(stubcc.masm);
        pic.slowPathStart = stubcc.linkExit(inlineJump, Uses(1));
        stubcc.leave();
        passICAddress(&pic);
        pic.slowPathCall = OOL_STUBCALL(ic::XName, REJOIN_GETTER);
        CHECK_OOL_SPACE();
        testPushedType(REJOIN_GETTER, -1);
    }

    pic.fastPathRejoin = masm.label();

    RETURN_IF_OOM(false);

    /* Initialize op labels. */
    ScopeNameLabels &labels = pic.scopeNameLabels();
    labels.setInlineJumpOffset(masm.differenceBetween(pic.fastPathStart, inlineJump));

    CHECK_IC_SPACE();

    frame.pop();
    frame.pushRegs(pic.shapeReg, pic.objReg, knownPushedType(0));

    BarrierState barrier = testBarrier(pic.shapeReg, pic.objReg, /* testUndefined = */ true);

    stubcc.rejoin(Changes(1));

    pics.append(pic);

    finishBarrier(barrier, REJOIN_FALLTHROUGH, 0);
    return true;
}

void
mjit::Compiler::jsop_bindname(PropertyName *name)
{
    /*
     * If this is a BINDNAME for a variable of a non-reentrant outer function,
     * the object is definitely the outer function's active call object.
     */
    if (cx->typeInferenceEnabled()) {
        ScriptAnalysis::NameAccess access =
            analysis->resolveNameAccess(cx, NameToId(name), true);
        if (access.nesting) {
            RegisterID reg = frame.allocReg();
            CallObject **pobj = &access.nesting->activeCall;
            masm.move(ImmPtr(pobj), reg);
            masm.loadPtr(Address(reg), reg);
            frame.pushTypedPayload(JSVAL_TYPE_OBJECT, reg);
            return;
        }
    }

    PICGenInfo pic(ic::PICInfo::BIND, JSOp(*PC));

    // This code does not check the frame flags to see if scopeChain has been
    // set. Rather, it relies on the up-front analysis statically determining
    // whether BINDNAME can be used, which reifies the scope chain at the
    // prologue.
    JS_ASSERT(analysis->usesScopeChain());

    pic.shapeReg = frame.allocReg();
    pic.objReg = frame.allocReg();
    pic.typeReg = Registers::ReturnReg;
    pic.name = name;
    pic.hasTypeCheck = false;

    RESERVE_IC_SPACE(masm);
    pic.fastPathStart = masm.label();

    masm.loadPtr(Address(JSFrameReg, StackFrame::offsetOfScopeChain()), pic.objReg);
    masm.loadPtr(Address(pic.objReg, JSObject::offsetOfShape()), pic.shapeReg);
    masm.loadPtr(Address(pic.shapeReg, Shape::offsetOfBase()), pic.shapeReg);
    Address parent(pic.shapeReg, BaseShape::offsetOfParent());

    pic.shapeGuard = masm.label();
    Jump inlineJump = masm.branchPtr(Assembler::NotEqual, parent, ImmPtr(NULL));
    {
        RESERVE_OOL_SPACE(stubcc.masm);
        pic.slowPathStart = stubcc.linkExit(inlineJump, Uses(0));
        stubcc.leave();
        passICAddress(&pic);
        pic.slowPathCall = OOL_STUBCALL(ic::BindName, REJOIN_FALLTHROUGH);
        CHECK_OOL_SPACE();
    }

    pic.fastPathRejoin = masm.label();

    /* Initialize op labels. */
    BindNameLabels &labels = pic.bindNameLabels();
    labels.setInlineJump(masm, pic.shapeGuard, inlineJump);

    frame.pushTypedPayload(JSVAL_TYPE_OBJECT, pic.objReg);
    frame.freeReg(pic.shapeReg);

    stubcc.rejoin(Changes(1));

    pics.append(pic);
}

#else /* !JS_POLYIC */

void
mjit::Compiler::jsop_name(PropertyName *name, JSValueType type, bool isCall)
{
    prepareStubCall(Uses(0));
    INLINE_STUBCALL(isCall ? stubs::CallName : stubs::Name, REJOIN_FALLTHROUGH);
    testPushedType(REJOIN_FALLTHROUGH, 0, /* ool = */ false);
    frame.pushSynced(type);
    if (isCall)
        frame.pushSynced(JSVAL_TYPE_UNKNOWN);
}

bool
mjit::Compiler::jsop_xname(PropertyName *name)
{
    return jsop_getprop(name, knownPushedType(0), pushedTypeSet(0));
}

bool
mjit::Compiler::jsop_getprop(PropertyName *name, JSValueType knownType, types::TypeSet *typeSet,
                             bool typecheck, bool forPrototype)
{
    jsop_getprop_slow(name, forPrototype);
    return true;
}

bool
mjit::Compiler::jsop_setprop(PropertyName *name)
{
    jsop_setprop_slow(name);
    return true;
}

void
mjit::Compiler::jsop_bindname(PropertyName *name)
{
    RegisterID reg = frame.allocReg();
    Address scopeChain(JSFrameReg, StackFrame::offsetOfScopeChain());
    masm.loadPtr(scopeChain, reg);

    Address address(reg, offsetof(JSObject, parent));

    Jump j = masm.branchPtr(Assembler::NotEqual, address, ImmPtr(0));

    stubcc.linkExit(j, Uses(0));
    stubcc.leave();
    stubcc.masm.move(ImmPtr(name), Registers::ArgReg1);
    OOL_STUBCALL(stubs::BindName, REJOIN_FALLTHROUGH);

    frame.pushTypedPayload(JSVAL_TYPE_OBJECT, reg);

    stubcc.rejoin(Changes(1));
}
#endif

void
mjit::Compiler::jsop_aliasedArg(unsigned arg, bool get, bool poppedAfter)
{
    RegisterID reg = frame.allocReg(Registers::SavedRegs).reg();
    masm.loadPtr(Address(JSFrameReg, StackFrame::offsetOfArgsObj()), reg);
    size_t dataOff = ArgumentsObject::getDataSlotOffset();
    masm.loadPrivate(Address(reg, dataOff), reg);
    int32_t argsOff = ArgumentsData::offsetOfArgs() + arg * sizeof(Value);
    masm.addPtr(Imm32(argsOff), reg, reg);
    if (get) {
        FrameEntry *fe = frame.getArg(arg);
        JSValueType type = fe->isTypeKnown() ? fe->getKnownType() : JSVAL_TYPE_UNKNOWN;
        frame.push(Address(reg), type, true /* = reuseBase */);
    } else {
#ifdef JSGC_INCREMENTAL_MJ
        if (cx->compartment->needsBarrier()) {
            /* Write barrier. */
            stubcc.linkExit(masm.testGCThing(Address(reg)), Uses(0));
            stubcc.leave();
            stubcc.masm.move(reg, Registers::ArgReg1);
            OOL_STUBCALL(stubs::GCThingWriteBarrier, REJOIN_NONE);
            stubcc.rejoin(Changes(0));
        }
#endif
        frame.storeTo(frame.peek(-1), Address(reg), poppedAfter);
        frame.freeReg(reg);
    }
}

void
mjit::Compiler::jsop_aliasedVar(ScopeCoordinate sc, bool get, bool poppedAfter)
{
    RegisterID reg = frame.allocReg(Registers::SavedRegs).reg();
    masm.loadPtr(Address(JSFrameReg, StackFrame::offsetOfScopeChain()), reg);
    for (unsigned i = 0; i < sc.hops; i++)
        masm.loadPayload(Address(reg, ScopeObject::offsetOfEnclosingScope()), reg);

    /*
     * TODO bug 753158: Call and Block objects should use the same layout
     * strategy: up to the maximum numFixedSlots and overflow (if any) in
     * dynamic slots. For now, we special case for different layouts:
     */
    Address addr;
    StaticBlockObject *block = ScopeCoordinateBlockChain(script, PC);
    if (block) {
        /*
         * Block objects use a fixed AllocKind which means an invariant number
         * of fixed slots. Any slot below the fixed slot count is inline, any
         * slot over is in the dynamic slots.
         */
        uint32_t nfixed = gc::GetGCKindSlots(BlockObject::FINALIZE_KIND);
        if (nfixed <= sc.slot) {
            masm.loadPtr(Address(reg, JSObject::offsetOfSlots()), reg);
            addr = Address(reg, (sc.slot - nfixed) * sizeof(Value));
        } else {
            addr = Address(reg, JSObject::getFixedSlotOffset(sc.slot));
        }
    } else {
        /*
         * Using special-case hackery in Shape::getChildBinding, CallObject
         * slots are either altogether in fixed slots or altogether in dynamic
         * slots (by having numFixed == RESERVED_SLOTS).
         */
        if (script->bindings.lastShape()->numFixedSlots() <= sc.slot) {
            masm.loadPtr(Address(reg, JSObject::offsetOfSlots()), reg);
            addr = Address(reg, (sc.slot - CallObject::RESERVED_SLOTS) * sizeof(Value));
        } else {
            addr = Address(reg, JSObject::getFixedSlotOffset(sc.slot));
        }
    }

    if (get) {
        unsigned index;
        FrameEntry *fe = ScopeCoordinateToFrameIndex(script, PC, &index) == FrameIndex_Local
                         ? frame.getLocal(index)
                         : frame.getArg(index);
        JSValueType type = fe->isTypeKnown() ? fe->getKnownType() : JSVAL_TYPE_UNKNOWN;
        frame.push(addr, type, true /* = reuseBase */);
    } else {
#ifdef JSGC_INCREMENTAL_MJ
        if (cx->compartment->needsBarrier()) {
            /* Write barrier. */
            stubcc.linkExit(masm.testGCThing(addr), Uses(0));
            stubcc.leave();
            stubcc.masm.addPtr(Imm32(addr.offset), addr.base, Registers::ArgReg1);
            OOL_STUBCALL(stubs::GCThingWriteBarrier, REJOIN_NONE);
            stubcc.rejoin(Changes(0));
        }
#endif
        frame.storeTo(frame.peek(-1), addr, poppedAfter);
        frame.freeReg(reg);
    }
}

void
mjit::Compiler::jsop_this()
{
    frame.pushThis();

    /*
     * In strict mode code, we don't wrap 'this'.
     * In direct-call eval code, we wrapped 'this' before entering the eval.
     * In global code, 'this' is always an object.
     */
    if (script->function() && !script->strictModeCode) {
        FrameEntry *thisFe = frame.peek(-1);

        if (!thisFe->isType(JSVAL_TYPE_OBJECT)) {
            /*
             * Watch out for an obscure case where we don't know we are pushing
             * an object: the script has not yet had a 'this' value assigned,
             * so no pushed 'this' type has been inferred. Don't mark the type
             * as known in this case, preserving the invariant that compiler
             * types reflect inferred types.
             */
            if (cx->typeInferenceEnabled() && knownPushedType(0) != JSVAL_TYPE_OBJECT) {
                prepareStubCall(Uses(1));
                INLINE_STUBCALL(stubs::This, REJOIN_FALLTHROUGH);
                return;
            }

            JSValueType type = cx->typeInferenceEnabled()
                ? types::TypeScript::ThisTypes(script)->getKnownTypeTag(cx)
                : JSVAL_TYPE_UNKNOWN;
            if (type != JSVAL_TYPE_OBJECT) {
                Jump notObj = frame.testObject(Assembler::NotEqual, thisFe);
                stubcc.linkExit(notObj, Uses(1));
                stubcc.leave();
                OOL_STUBCALL(stubs::This, REJOIN_FALLTHROUGH);
                stubcc.rejoin(Changes(1));
            }

            // Now we know that |this| is an object.
            frame.pop();
            frame.learnThisIsObject(type != JSVAL_TYPE_OBJECT);
            frame.pushThis();
        }

        JS_ASSERT(thisFe->isType(JSVAL_TYPE_OBJECT));
    }
}

bool
mjit::Compiler::iter(unsigned flags)
{
    FrameEntry *fe = frame.peek(-1);

    /*
     * Stub the call if this is not a simple 'for in' loop or if the iterated
     * value is known to not be an object.
     */
    if ((flags != JSITER_ENUMERATE) || fe->isNotType(JSVAL_TYPE_OBJECT)) {
        prepareStubCall(Uses(1));
        masm.move(Imm32(flags), Registers::ArgReg1);
        INLINE_STUBCALL(stubs::Iter, REJOIN_FALLTHROUGH);
        frame.pop();
        frame.pushSynced(JSVAL_TYPE_UNKNOWN);
        return true;
    }

    if (!fe->isTypeKnown()) {
        Jump notObject = frame.testObject(Assembler::NotEqual, fe);
        stubcc.linkExit(notObject, Uses(1));
    }

    frame.forgetMismatchedObject(fe);

    RegisterID reg = frame.tempRegForData(fe);

    frame.pinReg(reg);
    RegisterID ioreg = frame.allocReg();  /* Will hold iterator JSObject */
    RegisterID nireg = frame.allocReg();  /* Will hold NativeIterator */
    RegisterID T1 = frame.allocReg();
    RegisterID T2 = frame.allocReg();
    frame.unpinReg(reg);

    /* Fetch the most recent iterator. */
    masm.loadPtr(&cx->runtime->nativeIterCache.last, ioreg);

    /* Test for NULL. */
    Jump nullIterator = masm.branchTest32(Assembler::Zero, ioreg, ioreg);
    stubcc.linkExit(nullIterator, Uses(1));

    /* Get NativeIterator from iter obj. */
    masm.loadObjPrivate(ioreg, nireg, JSObject::ITER_CLASS_NFIXED_SLOTS);

    /* Test for active iterator. */
    Address flagsAddr(nireg, offsetof(NativeIterator, flags));
    masm.load32(flagsAddr, T1);
    Jump activeIterator = masm.branchTest32(Assembler::NonZero, T1,
                                            Imm32(JSITER_ACTIVE|JSITER_UNREUSABLE));
    stubcc.linkExit(activeIterator, Uses(1));

    /* Compare shape of object with iterator. */
    masm.loadShape(reg, T1);
    masm.loadPtr(Address(nireg, offsetof(NativeIterator, shapes_array)), T2);
    masm.loadPtr(Address(T2, 0), T2);
    Jump mismatchedObject = masm.branchPtr(Assembler::NotEqual, T1, T2);
    stubcc.linkExit(mismatchedObject, Uses(1));

    /* Compare shape of object's prototype with iterator. */
    masm.loadPtr(Address(reg, JSObject::offsetOfType()), T1);
    masm.loadPtr(Address(T1, offsetof(types::TypeObject, proto)), T1);
    masm.loadShape(T1, T1);
    masm.loadPtr(Address(nireg, offsetof(NativeIterator, shapes_array)), T2);
    masm.loadPtr(Address(T2, sizeof(Shape *)), T2);
    Jump mismatchedProto = masm.branchPtr(Assembler::NotEqual, T1, T2);
    stubcc.linkExit(mismatchedProto, Uses(1));

    /*
     * Compare object's prototype's prototype with NULL. The last native
     * iterator will always have a prototype chain length of one
     * (i.e. it must be a plain object), so we do not need to generate
     * a loop here.
     */
    masm.loadPtr(Address(reg, JSObject::offsetOfType()), T1);
    masm.loadPtr(Address(T1, offsetof(types::TypeObject, proto)), T1);
    masm.loadPtr(Address(T1, JSObject::offsetOfType()), T1);
    masm.loadPtr(Address(T1, offsetof(types::TypeObject, proto)), T1);
    Jump overlongChain = masm.branchPtr(Assembler::NonZero, T1, T1);
    stubcc.linkExit(overlongChain, Uses(1));

#ifdef JSGC_INCREMENTAL_MJ
    /*
     * Write barrier for stores to the iterator. We only need to take a write
     * barrier if NativeIterator::obj is actually going to change.
     */
    if (cx->compartment->needsBarrier()) {
        Jump j = masm.branchPtr(Assembler::NotEqual,
                                Address(nireg, offsetof(NativeIterator, obj)), reg);
        stubcc.linkExit(j, Uses(1));
    }
#endif

    /* Found a match with the most recent iterator. Hooray! */

    /* Mark iterator as active. */
    masm.storePtr(reg, Address(nireg, offsetof(NativeIterator, obj)));
    masm.load32(flagsAddr, T1);
    masm.or32(Imm32(JSITER_ACTIVE), T1);
    masm.store32(T1, flagsAddr);

    /* Chain onto the active iterator stack. */
    masm.loadPtr(FrameAddress(offsetof(VMFrame, cx)), T1);
    masm.loadPtr(Address(T1, offsetof(JSContext, enumerators)), T2);
    masm.storePtr(T2, Address(nireg, offsetof(NativeIterator, next)));
    masm.storePtr(ioreg, Address(T1, offsetof(JSContext, enumerators)));

    frame.freeReg(nireg);
    frame.freeReg(T1);
    frame.freeReg(T2);

    stubcc.leave();
    stubcc.masm.move(Imm32(flags), Registers::ArgReg1);
    OOL_STUBCALL(stubs::Iter, REJOIN_FALLTHROUGH);

    /* Push the iterator object. */
    frame.pop();
    frame.pushTypedPayload(JSVAL_TYPE_OBJECT, ioreg);

    stubcc.rejoin(Changes(1));

    return true;
}

/*
 * This big nasty function implements JSOP_ITERNEXT, which is used in the head
 * of a for-in loop to put the next value on the stack.
 */
void
mjit::Compiler::iterNext(ptrdiff_t offset)
{
    FrameEntry *fe = frame.peek(-offset);
    RegisterID reg = frame.tempRegForData(fe);

    /* Is it worth trying to pin this longer? Prolly not. */
    frame.pinReg(reg);
    RegisterID T1 = frame.allocReg();
    frame.unpinReg(reg);

    /* Test clasp */
    Jump notFast = masm.testObjClass(Assembler::NotEqual, reg, T1, &IteratorClass);
    stubcc.linkExit(notFast, Uses(1));

    /* Get private from iter obj. */
    masm.loadObjPrivate(reg, T1, JSObject::ITER_CLASS_NFIXED_SLOTS);

    RegisterID T3 = frame.allocReg();
    RegisterID T4 = frame.allocReg();

    /* Test for a value iterator, which could come through an Iterator object. */
    masm.load32(Address(T1, offsetof(NativeIterator, flags)), T3);
    notFast = masm.branchTest32(Assembler::NonZero, T3, Imm32(JSITER_FOREACH));
    stubcc.linkExit(notFast, Uses(1));

    RegisterID T2 = frame.allocReg();

    /* Get cursor. */
    masm.loadPtr(Address(T1, offsetof(NativeIterator, props_cursor)), T2);

    /* Get the next string in the iterator. */
    masm.loadPtr(T2, T3);

    /* It's safe to increase the cursor now. */
    masm.addPtr(Imm32(sizeof(JSString*)), T2, T4);
    masm.storePtr(T4, Address(T1, offsetof(NativeIterator, props_cursor)));

    frame.freeReg(T4);
    frame.freeReg(T1);
    frame.freeReg(T2);

    stubcc.leave();
    stubcc.masm.move(Imm32(offset), Registers::ArgReg1);
    OOL_STUBCALL(stubs::IterNext, REJOIN_FALLTHROUGH);

    frame.pushUntypedPayload(JSVAL_TYPE_STRING, T3);

    /* Join with the stub call. */
    stubcc.rejoin(Changes(1));
}

bool
mjit::Compiler::iterMore(jsbytecode *target)
{
    if (!frame.syncForBranch(target, Uses(1)))
        return false;

    FrameEntry *fe = frame.peek(-1);
    RegisterID reg = frame.tempRegForData(fe);
    RegisterID tempreg = frame.allocReg();

    /* Test clasp */
    Jump notFast = masm.testObjClass(Assembler::NotEqual, reg, tempreg, &IteratorClass);
    stubcc.linkExitForBranch(notFast);

    /* Get private from iter obj. */
    masm.loadObjPrivate(reg, reg, JSObject::ITER_CLASS_NFIXED_SLOTS);

    /* Test that the iterator supports fast iteration. */
    notFast = masm.branchTest32(Assembler::NonZero, Address(reg, offsetof(NativeIterator, flags)),
                                Imm32(JSITER_FOREACH));
    stubcc.linkExitForBranch(notFast);

    /* Get props_cursor, test */
    masm.loadPtr(Address(reg, offsetof(NativeIterator, props_cursor)), tempreg);
    masm.loadPtr(Address(reg, offsetof(NativeIterator, props_end)), reg);

    Jump jFast = masm.branchPtr(Assembler::LessThan, tempreg, reg);

    stubcc.leave();
    OOL_STUBCALL(stubs::IterMore, REJOIN_BRANCH);
    Jump j = stubcc.masm.branchTest32(Assembler::NonZero, Registers::ReturnReg,
                                      Registers::ReturnReg);

    stubcc.rejoin(Changes(1));
    frame.freeReg(tempreg);

    return jumpAndRun(jFast, target, &j);
}

void
mjit::Compiler::iterEnd()
{
    FrameEntry *fe= frame.peek(-1);
    RegisterID reg = frame.tempRegForData(fe);

    frame.pinReg(reg);
    RegisterID T1 = frame.allocReg();
    frame.unpinReg(reg);

    /* Test clasp */
    Jump notIterator = masm.testObjClass(Assembler::NotEqual, reg, T1, &IteratorClass);
    stubcc.linkExit(notIterator, Uses(1));

    /* Get private from iter obj. */
    masm.loadObjPrivate(reg, T1, JSObject::ITER_CLASS_NFIXED_SLOTS);

    RegisterID T2 = frame.allocReg();

    /* Load flags. */
    Address flagAddr(T1, offsetof(NativeIterator, flags));
    masm.loadPtr(flagAddr, T2);

    /* Test for a normal enumerate iterator. */
    Jump notEnumerate = masm.branchTest32(Assembler::Zero, T2, Imm32(JSITER_ENUMERATE));
    stubcc.linkExit(notEnumerate, Uses(1));

    /* Clear active bit. */
    masm.and32(Imm32(~JSITER_ACTIVE), T2);
    masm.storePtr(T2, flagAddr);

    /* Reset property cursor. */
    masm.loadPtr(Address(T1, offsetof(NativeIterator, props_array)), T2);
    masm.storePtr(T2, Address(T1, offsetof(NativeIterator, props_cursor)));

    /* Advance enumerators list. */
    masm.loadPtr(FrameAddress(offsetof(VMFrame, cx)), T2);
    masm.loadPtr(Address(T1, offsetof(NativeIterator, next)), T1);
    masm.storePtr(T1, Address(T2, offsetof(JSContext, enumerators)));

    frame.freeReg(T1);
    frame.freeReg(T2);

    stubcc.leave();
    OOL_STUBCALL(stubs::EndIter, REJOIN_FALLTHROUGH);

    frame.pop();

    stubcc.rejoin(Changes(1));
}

void
mjit::Compiler::jsop_getgname_slow(uint32_t index)
{
    prepareStubCall(Uses(0));
    INLINE_STUBCALL(stubs::Name, REJOIN_GETTER);
    testPushedType(REJOIN_GETTER, 0, /* ool = */ false);
    frame.pushSynced(JSVAL_TYPE_UNKNOWN);
}

void
mjit::Compiler::jsop_bindgname()
{
    if (globalObj) {
        frame.push(ObjectValue(*globalObj));
        return;
    }

    /* :TODO: this is slower than it needs to be. */
    prepareStubCall(Uses(0));
    INLINE_STUBCALL(stubs::BindGlobalName, REJOIN_NONE);
    frame.takeReg(Registers::ReturnReg);
    frame.pushTypedPayload(JSVAL_TYPE_OBJECT, Registers::ReturnReg);
}

bool
mjit::Compiler::jsop_getgname(uint32_t index)
{
    /* Optimize undefined, NaN and Infinity. */
    PropertyName *name = script->getName(index);
    if (name == cx->runtime->atomState.typeAtoms[JSTYPE_VOID]) {
        frame.push(UndefinedValue());
        return true;
    }
    if (name == cx->runtime->atomState.NaNAtom) {
        frame.push(cx->runtime->NaNValue);
        return true;
    }
    if (name == cx->runtime->atomState.InfinityAtom) {
        frame.push(cx->runtime->positiveInfinityValue);
        return true;
    }

    /* Optimize singletons like Math for JSOP_CALLPROP. */
    JSObject *obj = pushedSingleton(0);
    if (obj && !hasTypeBarriers(PC)) {
        Rooted<jsid> id(cx, NameToId(name));
        if (testSingletonProperty(globalObj, id)) {
            frame.push(ObjectValue(*obj));
            return true;
        }
    }

    jsid id = NameToId(name);
    JSValueType type = knownPushedType(0);
    if (cx->typeInferenceEnabled() && globalObj->isGlobal() && id == types::MakeTypeId(cx, id) &&
        !globalObj->getType(cx)->unknownProperties()) {
        types::TypeSet *propertyTypes = globalObj->getType(cx)->getProperty(cx, id, false);
        if (!propertyTypes)
            return false;

        /*
         * If we are accessing a defined global which is a normal data property
         * then bake its address into the jitcode and guard against future
         * reallocation of the global object's slots.
         */
        js::Shape *shape = globalObj->nativeLookup(cx, NameToId(name));
        if (shape && shape->hasDefaultGetter() && shape->hasSlot()) {
            HeapSlot *value = &globalObj->getSlotRef(shape->slot());
            if (!value->isUndefined() &&
                !propertyTypes->isOwnProperty(cx, globalObj->getType(cx), true)) {
                watchGlobalReallocation();
                RegisterID reg = frame.allocReg();
                masm.move(ImmPtr(value), reg);

                BarrierState barrier = pushAddressMaybeBarrier(Address(reg), type, true);
                finishBarrier(barrier, REJOIN_GETTER, 0);
                return true;
            }
        }
    }

#if defined JS_MONOIC
    jsop_bindgname();

    FrameEntry *fe = frame.peek(-1);
    JS_ASSERT(fe->isTypeKnown() && fe->getKnownType() == JSVAL_TYPE_OBJECT);

    GetGlobalNameICInfo ic;
    RESERVE_IC_SPACE(masm);
    RegisterID objReg;
    Jump shapeGuard;

    ic.fastPathStart = masm.label();
    if (fe->isConstant()) {
        JSObject *obj = &fe->getValue().toObject();
        frame.pop();
        JS_ASSERT(obj->isNative());

        objReg = frame.allocReg();

        masm.loadPtrFromImm(obj->addressOfShape(), objReg);
        shapeGuard = masm.branchPtrWithPatch(Assembler::NotEqual, objReg,
                                             ic.shape, ImmPtr(NULL));
        masm.move(ImmPtr(obj), objReg);
    } else {
        objReg = frame.ownRegForData(fe);
        frame.pop();
        RegisterID reg = frame.allocReg();

        masm.loadShape(objReg, reg);
        shapeGuard = masm.branchPtrWithPatch(Assembler::NotEqual, reg,
                                             ic.shape, ImmPtr(NULL));
        frame.freeReg(reg);
    }
    stubcc.linkExit(shapeGuard, Uses(0));

    stubcc.leave();
    passMICAddress(ic);
    ic.slowPathCall = OOL_STUBCALL(ic::GetGlobalName, REJOIN_GETTER);

    CHECK_IC_SPACE();

    testPushedType(REJOIN_GETTER, 0);

    /* Garbage value. */
    uint32_t slot = 1 << 24;

    masm.loadPtr(Address(objReg, JSObject::offsetOfSlots()), objReg);
    Address address(objReg, slot);

    /* Allocate any register other than objReg. */
    RegisterID treg = frame.allocReg();
    /* After dreg is loaded, it's safe to clobber objReg. */
    RegisterID dreg = objReg;

    ic.load = masm.loadValueWithAddressOffsetPatch(address, treg, dreg);

    frame.pushRegs(treg, dreg, type);

    /*
     * Note: no undefined check is needed for GNAME opcodes. These were not
     * declared with 'var', so cannot be undefined without triggering an error
     * or having been a pre-existing global whose value is undefined (which
     * type inference will know about).
     */
    BarrierState barrier = testBarrier(treg, dreg);

    stubcc.rejoin(Changes(1));

    getGlobalNames.append(ic);
    finishBarrier(barrier, REJOIN_GETTER, 0);
#else
    jsop_getgname_slow(index);
#endif
    return true;
}

void
mjit::Compiler::jsop_setgname_slow(PropertyName *name)
{
    prepareStubCall(Uses(2));
    masm.move(ImmPtr(name), Registers::ArgReg1);
    INLINE_STUBCALL(STRICT_VARIANT(stubs::SetGlobalName), REJOIN_FALLTHROUGH);
    frame.popn(2);
    pushSyncedEntry(0);
}

bool
mjit::Compiler::jsop_setgname(PropertyName *name, bool popGuaranteed)
{
    if (monitored(PC)) {
        /* Global accesses are monitored only for a few names like __proto__. */
        jsop_setgname_slow(name);
        return true;
    }

    jsid id = NameToId(name);
    if (cx->typeInferenceEnabled() && globalObj->isGlobal() && id == types::MakeTypeId(cx, id) &&
        !globalObj->getType(cx)->unknownProperties()) {
        /*
         * Note: object branding is disabled when inference is enabled. With
         * branding there is no way to ensure that a non-function property
         * can't get a function later and cause the global object to become
         * branded, requiring a shape change if it changes again.
         */
        types::TypeSet *types = globalObj->getType(cx)->getProperty(cx, id, false);
        if (!types)
            return false;
        js::Shape *shape = globalObj->nativeLookup(cx, NameToId(name));
        if (shape && shape->hasDefaultSetter() &&
            shape->writable() && shape->hasSlot() &&
            !types->isOwnProperty(cx, globalObj->getType(cx), true)) {
            watchGlobalReallocation();
            HeapSlot *value = &globalObj->getSlotRef(shape->slot());
            RegisterID reg = frame.allocReg();
#ifdef JSGC_INCREMENTAL_MJ
            /* Write barrier. */
            if (cx->compartment->needsBarrier() && types->needsBarrier(cx)) {
                stubcc.linkExit(masm.jump(), Uses(0));
                stubcc.leave();
                stubcc.masm.move(ImmPtr(value), Registers::ArgReg1);
                OOL_STUBCALL(stubs::WriteBarrier, REJOIN_NONE);
                stubcc.rejoin(Changes(0));
            }
#endif
            masm.move(ImmPtr(value), reg);
            frame.storeTo(frame.peek(-1), Address(reg), popGuaranteed);
            frame.shimmy(1);
            frame.freeReg(reg);
            return true;
        }
    }

#ifdef JSGC_INCREMENTAL_MJ
    /* Write barrier. */
    if (cx->compartment->needsBarrier()) {
        jsop_setgname_slow(name);
        return true;
    }
#endif

#if defined JS_MONOIC
    FrameEntry *objFe = frame.peek(-2);
    FrameEntry *fe = frame.peek(-1);
    JS_ASSERT_IF(objFe->isTypeKnown(), objFe->getKnownType() == JSVAL_TYPE_OBJECT);

    if (!fe->isConstant() && fe->isType(JSVAL_TYPE_DOUBLE))
        frame.forgetKnownDouble(fe);

    SetGlobalNameICInfo ic;

    frame.pinEntry(fe, ic.vr);
    Jump shapeGuard;

    RESERVE_IC_SPACE(masm);

    ic.fastPathStart = masm.label();
    if (objFe->isConstant()) {
        JSObject *obj = &objFe->getValue().toObject();
        JS_ASSERT(obj->isNative());

        ic.objReg = frame.allocReg();
        ic.shapeReg = ic.objReg;
        ic.objConst = true;

        masm.loadPtrFromImm(obj->addressOfShape(), ic.shapeReg);
        shapeGuard = masm.branchPtrWithPatch(Assembler::NotEqual, ic.shapeReg,
                                             ic.shape, ImmPtr(NULL));
        masm.move(ImmPtr(obj), ic.objReg);
    } else {
        ic.objReg = frame.copyDataIntoReg(objFe);
        ic.shapeReg = frame.allocReg();
        ic.objConst = false;

        masm.loadShape(ic.objReg, ic.shapeReg);
        shapeGuard = masm.branchPtrWithPatch(Assembler::NotEqual, ic.shapeReg,
                                             ic.shape, ImmPtr(NULL));
        frame.freeReg(ic.shapeReg);
    }
    ic.shapeGuardJump = shapeGuard;
    ic.slowPathStart = stubcc.linkExit(shapeGuard, Uses(2));

    stubcc.leave();
    passMICAddress(ic);
    ic.slowPathCall = OOL_STUBCALL(ic::SetGlobalName, REJOIN_FALLTHROUGH);

    /* Garbage value. */
    uint32_t slot = 1 << 24;

    masm.loadPtr(Address(ic.objReg, JSObject::offsetOfSlots()), ic.objReg);
    Address address(ic.objReg, slot);

    if (ic.vr.isConstant()) {
        ic.store = masm.storeValueWithAddressOffsetPatch(ic.vr.value(), address);
    } else if (ic.vr.isTypeKnown()) {
        ic.store = masm.storeValueWithAddressOffsetPatch(ImmType(ic.vr.knownType()),
                                                          ic.vr.dataReg(), address);
    } else {
        ic.store = masm.storeValueWithAddressOffsetPatch(ic.vr.typeReg(), ic.vr.dataReg(), address);
    }

    frame.freeReg(ic.objReg);
    frame.unpinEntry(ic.vr);
    frame.shimmy(1);

    stubcc.rejoin(Changes(1));

    ic.fastPathRejoin = masm.label();
    setGlobalNames.append(ic);
#else
    jsop_setgname_slow(name);
#endif
    return true;
}

void
mjit::Compiler::jsop_setelem_slow()
{
    prepareStubCall(Uses(3));
    INLINE_STUBCALL(STRICT_VARIANT(stubs::SetElem), REJOIN_FALLTHROUGH);
    frame.popn(3);
    frame.pushSynced(JSVAL_TYPE_UNKNOWN);
}

void
mjit::Compiler::jsop_getelem_slow()
{
    prepareStubCall(Uses(2));
    INLINE_STUBCALL(stubs::GetElem, REJOIN_FALLTHROUGH);
    testPushedType(REJOIN_FALLTHROUGH, -2, /* ool = */ false);
    frame.popn(2);
    pushSyncedEntry(0);
}

bool
mjit::Compiler::jsop_instanceof()
{
    FrameEntry *lhs = frame.peek(-2);
    FrameEntry *rhs = frame.peek(-1);

    // The fast path applies only when both operands are objects.
    if (rhs->isNotType(JSVAL_TYPE_OBJECT) || lhs->isNotType(JSVAL_TYPE_OBJECT)) {
        stubcc.linkExit(masm.jump(), Uses(2));
        frame.discardFe(lhs);
        frame.discardFe(rhs);
    }

    MaybeJump firstSlow;
    if (!rhs->isTypeKnown()) {
        Jump j = frame.testObject(Assembler::NotEqual, rhs);
        stubcc.linkExit(j, Uses(2));
    }

    frame.forgetMismatchedObject(lhs);
    frame.forgetMismatchedObject(rhs);

    RegisterID tmp = frame.allocReg();
    RegisterID obj = frame.tempRegForData(rhs);

    masm.loadBaseShape(obj, tmp);
    Jump notFunction = masm.branchPtr(Assembler::NotEqual,
                                      Address(tmp, BaseShape::offsetOfClass()),
                                      ImmPtr(&FunctionClass));

    stubcc.linkExit(notFunction, Uses(2));

    /* Test for bound functions. */
    Jump isBound = masm.branchTest32(Assembler::NonZero,
                                     Address(tmp, BaseShape::offsetOfFlags()),
                                     Imm32(BaseShape::BOUND_FUNCTION));
    {
        stubcc.linkExit(isBound, Uses(2));
        stubcc.leave();
        OOL_STUBCALL(stubs::InstanceOf, REJOIN_FALLTHROUGH);
        firstSlow = stubcc.masm.jump();
    }

    frame.freeReg(tmp);

    /* This is sadly necessary because the error case needs the object. */
    frame.dup();

    if (!jsop_getprop(cx->runtime->atomState.classPrototypeAtom, JSVAL_TYPE_UNKNOWN))
        return false;

    /* Primitive prototypes are invalid. */
    rhs = frame.peek(-1);
    Jump j = frame.testPrimitive(Assembler::Equal, rhs);
    stubcc.linkExit(j, Uses(3));

    /* Allocate registers up front, because of branchiness. */
    obj = frame.copyDataIntoReg(lhs);
    RegisterID proto = frame.copyDataIntoReg(rhs);
    RegisterID temp = frame.allocReg();

    MaybeJump isFalse;
    if (!lhs->isTypeKnown())
        isFalse = frame.testPrimitive(Assembler::Equal, lhs);

    Label loop = masm.label();

    /* Walk prototype chain, break out on NULL or hit. */
    masm.loadPtr(Address(obj, JSObject::offsetOfType()), obj);
    masm.loadPtr(Address(obj, offsetof(types::TypeObject, proto)), obj);
    Jump isFalse2 = masm.branchTestPtr(Assembler::Zero, obj, obj);
    Jump isTrue = masm.branchPtr(Assembler::NotEqual, obj, proto);
    isTrue.linkTo(loop, &masm);
    masm.move(Imm32(1), temp);
    isTrue = masm.jump();

    if (isFalse.isSet())
        isFalse.getJump().linkTo(masm.label(), &masm);
    isFalse2.linkTo(masm.label(), &masm);
    masm.move(Imm32(0), temp);
    isTrue.linkTo(masm.label(), &masm);

    frame.freeReg(proto);
    frame.freeReg(obj);

    stubcc.leave();
    OOL_STUBCALL(stubs::FastInstanceOf, REJOIN_FALLTHROUGH);

    frame.popn(3);
    frame.pushTypedPayload(JSVAL_TYPE_BOOLEAN, temp);

    if (firstSlow.isSet())
        firstSlow.getJump().linkTo(stubcc.masm.label(), &stubcc.masm);
    stubcc.rejoin(Changes(1));
    return true;
}

void
mjit::Compiler::emitEval(uint32_t argc)
{
    /* Check for interrupts on function call */
    interruptCheckHelper();

    frame.syncAndKill(Uses(argc + 2));
    prepareStubCall(Uses(argc + 2));
    masm.move(Imm32(argc), Registers::ArgReg1);
    INLINE_STUBCALL(stubs::Eval, REJOIN_FALLTHROUGH);
    frame.popn(argc + 2);
    pushSyncedEntry(0);
}

Compiler::Jump
Compiler::getNewObject(JSContext *cx, RegisterID result, JSObject *templateObject)
{
    rootedTemplates.append(templateObject);
    return masm.getNewObject(cx, result, templateObject);
}

bool
mjit::Compiler::jsop_newinit()
{
    bool isArray;
    unsigned count = 0;
    RootedObject baseobj(cx);
    switch (*PC) {
      case JSOP_NEWINIT:
        isArray = (GET_UINT8(PC) == JSProto_Array);
        break;
      case JSOP_NEWARRAY:
        isArray = true;
        count = GET_UINT24(PC);
        break;
      case JSOP_NEWOBJECT:
        /*
         * Scripts with NEWOBJECT must be compileAndGo, but treat these like
         * NEWINIT if the script's associated global is not known (or is not
         * actually a global object). This should only happen in chrome code.
         */
        isArray = false;
        baseobj = globalObj ? script->getObject(GET_UINT32_INDEX(PC)) : NULL;
        break;
      default:
        JS_NOT_REACHED("Bad op");
        return false;
    }

    void *stub, *stubArg;
    if (isArray) {
        stub = JS_FUNC_TO_DATA_PTR(void *, stubs::NewInitArray);
        stubArg = (void *) uintptr_t(count);
    } else {
        stub = JS_FUNC_TO_DATA_PTR(void *, stubs::NewInitObject);
        stubArg = (void *) baseobj;
    }

    JSProtoKey key = isArray ? JSProto_Array : JSProto_Object;

    /*
     * Don't bake in types for non-compileAndGo scripts, or at initializers
     * producing objects with singleton types.
     */
    RootedTypeObject type(cx);
    if (globalObj && !types::UseNewTypeForInitializer(cx, script, PC, key)) {
        type = types::TypeScript::InitObject(cx, script, PC, key);
        if (!type)
            return false;
    }

    size_t maxArraySlots =
        gc::GetGCKindSlots(gc::FINALIZE_OBJECT_LAST) - ObjectElements::VALUES_PER_HEADER;

    if (!cx->typeInferenceEnabled() ||
        !type ||
        (isArray && count > maxArraySlots) ||
        (!isArray && !baseobj) ||
        (!isArray && baseobj->hasDynamicSlots())) {
        prepareStubCall(Uses(0));
        masm.storePtr(ImmPtr(type), FrameAddress(offsetof(VMFrame, scratch)));
        masm.move(ImmPtr(stubArg), Registers::ArgReg1);
        INLINE_STUBCALL(stub, REJOIN_FALLTHROUGH);
        frame.pushSynced(JSVAL_TYPE_OBJECT);

        frame.extra(frame.peek(-1)).initArray = (*PC == JSOP_NEWARRAY);
        frame.extra(frame.peek(-1)).initObject = baseobj;

        return true;
    }

    JSObject *templateObject;
    if (isArray)
        templateObject = NewDenseUnallocatedArray(cx, count);
    else
        templateObject = CopyInitializerObject(cx, baseobj);
    if (!templateObject)
        return false;
    templateObject->setType(type);

    RegisterID result = frame.allocReg();
    Jump emptyFreeList = getNewObject(cx, result, templateObject);

    stubcc.linkExit(emptyFreeList, Uses(0));
    stubcc.leave();

    stubcc.masm.storePtr(ImmPtr(type), FrameAddress(offsetof(VMFrame, scratch)));
    stubcc.masm.move(ImmPtr(stubArg), Registers::ArgReg1);
    OOL_STUBCALL(stub, REJOIN_FALLTHROUGH);

    frame.pushTypedPayload(JSVAL_TYPE_OBJECT, result);

    stubcc.rejoin(Changes(1));

    frame.extra(frame.peek(-1)).initArray = (*PC == JSOP_NEWARRAY);
    frame.extra(frame.peek(-1)).initObject = baseobj;

    return true;
}

bool
mjit::Compiler::jsop_regexp()
{
    JSObject *obj = script->getRegExp(GET_UINT32_INDEX(PC));
    RegExpStatics *res = globalObj ? globalObj->getRegExpStatics() : NULL;

    if (!globalObj ||
        &obj->global() != globalObj ||
        !cx->typeInferenceEnabled() ||
        analysis->localsAliasStack() ||
        types::TypeSet::HasObjectFlags(cx, globalObj->getType(cx),
                                       types::OBJECT_FLAG_REGEXP_FLAGS_SET))
    {
        prepareStubCall(Uses(0));
        masm.move(ImmPtr(obj), Registers::ArgReg1);
        INLINE_STUBCALL(stubs::RegExp, REJOIN_FALLTHROUGH);
        frame.pushSynced(JSVAL_TYPE_OBJECT);
        return true;
    }

    RegExpObject *reobj = &obj->asRegExp();

    DebugOnly<uint32_t> origFlags = reobj->getFlags();
    DebugOnly<uint32_t> staticsFlags = res->getFlags();
    JS_ASSERT((origFlags & staticsFlags) == staticsFlags);

    /*
     * JS semantics require regular expression literals to create different
     * objects every time they execute. We only need to do this cloning if the
     * script could actually observe the effect of such cloning, by getting
     * or setting properties on it. Particular RegExp and String natives take
     * regular expressions as 'this' or an argument, and do not let that
     * expression escape and be accessed by the script, so avoid cloning in
     * these cases.
     */
    analyze::SSAUseChain *uses =
        analysis->useChain(analyze::SSAValue::PushedValue(PC - script->code, 0));
    if (uses && uses->popped && !uses->next && !reobj->global() && !reobj->sticky()) {
        jsbytecode *use = script->code + uses->offset;
        uint32_t which = uses->u.which;
        if (JSOp(*use) == JSOP_CALLPROP) {
            JSObject *callee = analysis->pushedTypes(use, 0)->getSingleton(cx);
            if (callee && callee->isFunction()) {
                Native native = callee->toFunction()->maybeNative();
                if (native == js::regexp_exec || native == js::regexp_test) {
                    frame.push(ObjectValue(*obj));
                    return true;
                }
            }
        } else if (JSOp(*use) == JSOP_CALL && which == 0) {
            uint32_t argc = GET_ARGC(use);
            JSObject *callee = analysis->poppedTypes(use, argc + 1)->getSingleton(cx);
            if (callee && callee->isFunction() && argc >= 1 && which == argc - 1) {
                Native native = callee->toFunction()->maybeNative();
                if (native == js::str_match ||
                    native == js::str_search ||
                    native == js::str_replace ||
                    native == js::str_split) {
                    frame.push(ObjectValue(*obj));
                    return true;
                }
            }
        }
    }

    /*
     * Force creation of the RegExpShared in the script's RegExpObject so that
     * we grab it in the getNewObject template copy. A strong reference to the
     * RegExpShared will be added when the jitcode is created. Any GC activity
     * between now and construction of that jitcode could purge the shared
     * info, but such activity will also abort compilation.
     */
    RegExpGuard g;
    if (!reobj->getShared(cx, &g))
        return false;

    rootedRegExps.append(g.re());

    RegisterID result = frame.allocReg();
    Jump emptyFreeList = getNewObject(cx, result, obj);

    stubcc.linkExit(emptyFreeList, Uses(0));
    stubcc.leave();

    stubcc.masm.move(ImmPtr(obj), Registers::ArgReg1);
    OOL_STUBCALL(stubs::RegExp, REJOIN_FALLTHROUGH);

    frame.pushTypedPayload(JSVAL_TYPE_OBJECT, result);

    stubcc.rejoin(Changes(1));
    return true;
}

bool
mjit::Compiler::startLoop(jsbytecode *head, Jump entry, jsbytecode *entryTarget)
{
    JS_ASSERT(cx->typeInferenceEnabled() && script == outerScript);
    JS_ASSERT(shouldStartLoop(head));

    if (loop) {
        /*
         * Convert all loop registers in the outer loop into unassigned registers.
         * We don't keep track of which registers the inner loop uses, so the only
         * registers that can be carried in the outer loop must be mentioned before
         * the inner loop starts.
         */
        loop->clearLoopRegisters();
    }

    LoopState *nloop = OffTheBooks::new_<LoopState>(cx, &ssa, this, &frame);
    if (!nloop || !nloop->init(head, entry, entryTarget)) {
        js_ReportOutOfMemory(cx);
        return false;
    }

    nloop->outer = loop;
    loop = nloop;
    frame.setLoop(loop);

    return true;
}

bool
mjit::Compiler::finishLoop(jsbytecode *head)
{
    if (!cx->typeInferenceEnabled() || !bytecodeInChunk(head))
        return true;

    /*
     * We're done processing the current loop. Every loop has exactly one backedge
     * at the end ('continue' statements are forward jumps to the loop test),
     * and after jumpAndRun'ing on that edge we can pop it from the frame.
     */
    JS_ASSERT(loop && loop->headOffset() == uint32_t(head - script->code));

    jsbytecode *entryTarget = script->code + loop->entryOffset();

    /*
     * Fix up the jump entering the loop. We are doing this after all code has
     * been emitted for the backedge, so that we are now in the loop's fallthrough
     * (where we will emit the entry code).
     */
    Jump fallthrough = masm.jump();

#ifdef DEBUG
    if (IsJaegerSpewChannelActive(JSpew_Regalloc)) {
        RegisterAllocation *alloc = analysis->getAllocation(head);
        JaegerSpew(JSpew_Regalloc, "loop allocation at %u:", unsigned(head - script->code));
        frame.dumpAllocation(alloc);
    }
#endif

    loop->entryJump().linkTo(masm.label(), &masm);

    jsbytecode *oldPC = PC;

    PC = entryTarget;
    {
        OOL_STUBCALL(stubs::MissedBoundsCheckEntry, REJOIN_RESUME);

        if (loop->generatingInvariants()) {
            /*
             * To do the initial load of the invariants, jump to the invariant
             * restore point after the call just emitted. :XXX: fix hackiness.
             */
            if (oomInVector)
                return false;
            Label label = callSites[callSites.length() - 1].loopJumpLabel;
            stubcc.linkExitDirect(masm.jump(), label);
        }
        stubcc.crossJump(stubcc.masm.jump(), masm.label());
    }
    PC = oldPC;

    frame.prepareForJump(entryTarget, masm, true);

    if (!jumpInScript(masm.jump(), entryTarget))
        return false;

    PC = head;
    if (!analysis->getCode(head).safePoint) {
        /*
         * Emit a stub into the OOL path which loads registers from a synced state
         * and jumps to the loop head, for rejoining from the interpreter.
         */
        LoopEntry entry;
        entry.pcOffset = head - script->code;

        OOL_STUBCALL(stubs::MissedBoundsCheckHead, REJOIN_RESUME);

        if (loop->generatingInvariants()) {
            if (oomInVector)
                return false;
            entry.label = callSites[callSites.length() - 1].loopJumpLabel;
        } else {
            entry.label = stubcc.masm.label();
        }

        /*
         * The interpreter may store integers in slots we assume are doubles,
         * make sure state is consistent before joining. Note that we don't
         * need any handling for other safe points the interpreter can enter
         * from, i.e. from switch and try blocks, as we don't assume double
         * variables are coherent in such cases.
         */
        for (uint32_t slot = ArgSlot(0); slot < TotalSlots(script); slot++) {
            if (a->varTypes[slot].getTypeTag(cx) == JSVAL_TYPE_DOUBLE) {
                FrameEntry *fe = frame.getSlotEntry(slot);
                stubcc.masm.ensureInMemoryDouble(frame.addressOf(fe));
            }
        }

        /*
         * Also watch for slots which we assume are doubles at the loop head,
         * but are known to be int32s at this point.
         */
        const SlotValue *newv = analysis->newValues(head);
        if (newv) {
            while (newv->slot) {
                if (newv->value.kind() == SSAValue::PHI &&
                    newv->value.phiOffset() == uint32_t(head - script->code) &&
                    analysis->trackSlot(newv->slot))
                {
                    JS_ASSERT(newv->slot < TotalSlots(script));
                    types::TypeSet *targetTypes = analysis->getValueTypes(newv->value);
                    if (targetTypes->getKnownTypeTag(cx) == JSVAL_TYPE_DOUBLE) {
                        FrameEntry *fe = frame.getSlotEntry(newv->slot);
                        stubcc.masm.ensureInMemoryDouble(frame.addressOf(fe));
                    }
                }
                newv++;
            }
        }

        frame.prepareForJump(head, stubcc.masm, true);
        if (!stubcc.jumpInScript(stubcc.masm.jump(), head))
            return false;

        loopEntries.append(entry);
    }
    PC = oldPC;

    /* Write out loads and tests of loop invariants at all calls in the loop body. */
    loop->flushLoop(stubcc);

    LoopState *nloop = loop->outer;
    cx->delete_(loop);
    loop = nloop;
    frame.setLoop(loop);

    fallthrough.linkTo(masm.label(), &masm);

    /*
     * Clear all registers used for loop temporaries. In the case of loop
     * nesting, we do not allocate temporaries for the outer loop.
     */
    frame.clearTemporaries();

    return true;
}

/*
 * The state at the fast jump must reflect the frame's current state. If specified
 * the state at the slow jump must be fully synced.
 *
 * The 'trampoline' argument indicates whether a trampoline was emitted into
 * the OOL path loading some registers for the target. If this is the case,
 * the fast path jump was redirected to the stub code's initial label, and the
 * same must happen for any other fast paths for the target (i.e. paths from
 * inline caches).
 *
 * The 'fallthrough' argument indicates this is a jump emitted for a fallthrough
 * at the end of the compiled chunk. In this case the opcode may not be a
 * JOF_JUMP opcode, and the compiler should not watch for fusions.
 */
bool
mjit::Compiler::jumpAndRun(Jump j, jsbytecode *target, Jump *slow, bool *trampoline,
                           bool fallthrough)
{
    if (trampoline)
        *trampoline = false;

    if (!a->parent && !bytecodeInChunk(target)) {
        /*
         * syncForBranch() must have ensured the stack is synced. Figure out
         * the source of the jump, which may be the opcode after PC if two ops
         * were fused for a branch.
         */
        OutgoingChunkEdge edge;
        edge.source = PC - outerScript->code;
        JSOp op = JSOp(*PC);
        if (!fallthrough && !(js_CodeSpec[op].format & JOF_JUMP) && op != JSOP_TABLESWITCH)
            edge.source += GetBytecodeLength(PC);
        edge.target = target - outerScript->code;
        edge.fastJump = j;
        if (slow)
            edge.slowJump = *slow;
        chunkEdges.append(edge);
        return true;
    }

    /*
     * Unless we are coming from a branch which synced everything, syncForBranch
     * must have been called and ensured an allocation at the target.
     */
    RegisterAllocation *lvtarget = NULL;
    bool consistent = true;
    if (cx->typeInferenceEnabled()) {
        RegisterAllocation *&alloc = analysis->getAllocation(target);
        if (!alloc) {
            alloc = cx->typeLifoAlloc().new_<RegisterAllocation>(false);
            if (!alloc) {
                js_ReportOutOfMemory(cx);
                return false;
            }
        }
        lvtarget = alloc;
        consistent = frame.consistentRegisters(target);
    }

    if (!lvtarget || lvtarget->synced()) {
        JS_ASSERT(consistent);
        if (!jumpInScript(j, target))
            return false;
        if (slow && !stubcc.jumpInScript(*slow, target))
            return false;
    } else {
        if (consistent) {
            if (!jumpInScript(j, target))
                return false;
        } else {
            /*
             * Make a trampoline to issue remaining loads for the register
             * state at target.
             */
            Label start = stubcc.masm.label();
            stubcc.linkExitDirect(j, start);
            frame.prepareForJump(target, stubcc.masm, false);
            if (!stubcc.jumpInScript(stubcc.masm.jump(), target))
                return false;
            if (trampoline)
                *trampoline = true;
            if (pcLengths) {
                /*
                 * This is OOL code but will usually be executed, so track
                 * it in the CODE_LENGTH for the opcode.
                 */
                uint32_t offset = ssa.frameLength(a->inlineIndex) + PC - script->code;
                size_t length = stubcc.masm.size() - stubcc.masm.distanceOf(start);
                pcLengths[offset].codeLength += length;
            }
        }

        if (slow) {
            slow->linkTo(stubcc.masm.label(), &stubcc.masm);
            frame.prepareForJump(target, stubcc.masm, true);
            if (!stubcc.jumpInScript(stubcc.masm.jump(), target))
                return false;
        }
    }

    if (target < PC)
        return finishLoop(target);
    return true;
}

void
mjit::Compiler::enterBlock(StaticBlockObject *block)
{
    /* For now, don't bother doing anything for this opcode. */
    frame.syncAndForgetEverything();
    masm.move(ImmPtr(block), Registers::ArgReg1);
    INLINE_STUBCALL(stubs::EnterBlock, REJOIN_FALLTHROUGH);
    if (*PC == JSOP_ENTERBLOCK)
        frame.enterBlock(StackDefs(script, PC));
}

void
mjit::Compiler::leaveBlock()
{
    /*
     * Note: After bug 535912, we can pass the block obj directly, inline
     * PutBlockObject, and do away with the muckiness in PutBlockObject.
     */
    uint32_t n = StackUses(script, PC);
    prepareStubCall(Uses(n));
    INLINE_STUBCALL(stubs::LeaveBlock, REJOIN_NONE);
    frame.leaveBlock(n);
}

// Creates the new object expected for constructors, and places it in |thisv|.
// It is broken down into the following operations:
//   CALLEE
//   GETPROP "prototype"
//   IFPRIMTOP:
//       NULL
//   call js_CreateThisFromFunctionWithProto(...)
//
bool
mjit::Compiler::constructThis()
{
    JS_ASSERT(isConstructing);

    RootedFunction fun(cx, script->function());

    do {
        if (!cx->typeInferenceEnabled() ||
            !fun->hasSingletonType() ||
            fun->getType(cx)->unknownProperties())
        {
            break;
        }

        jsid id = NameToId(cx->runtime->atomState.classPrototypeAtom);
        types::TypeSet *protoTypes = fun->getType(cx)->getProperty(cx, id, false);

        JSObject *proto = protoTypes->getSingleton(cx, true);
        if (!proto)
            break;

        /*
         * Generate an inline path to create a 'this' object with the given
         * prototype. Only do this if the type is actually known as a possible
         * 'this' type of the script.
         */
        types::TypeObject *type = proto->getNewType(cx, fun);
        if (!type)
            return false;
        if (!types::TypeScript::ThisTypes(script)->hasType(types::Type::ObjectType(type)))
            break;

        JSObject *templateObject = js_CreateThisForFunctionWithProto(cx, fun, proto);
        if (!templateObject)
            return false;

        /*
         * The template incorporates a shape and/or fixed slots from any
         * newScript on its type, so make sure recompilation is triggered
         * should this information change later.
         */
        if (templateObject->type()->newScript)
            types::TypeSet::WatchObjectStateChange(cx, templateObject->type());

        RegisterID result = frame.allocReg();
        Jump emptyFreeList = getNewObject(cx, result, templateObject);

        stubcc.linkExit(emptyFreeList, Uses(0));
        stubcc.leave();

        stubcc.masm.move(ImmPtr(proto), Registers::ArgReg1);
        OOL_STUBCALL(stubs::CreateThis, REJOIN_RESUME);

        frame.setThis(result);

        stubcc.rejoin(Changes(1));
        return true;
    } while (false);

    // Load the callee.
    frame.pushCallee();

    // Get callee.prototype.
    if (!jsop_getprop(cx->runtime->atomState.classPrototypeAtom, JSVAL_TYPE_UNKNOWN, false, /* forPrototype = */ true))
        return false;

    // Reach into the proto Value and grab a register for its data.
    FrameEntry *protoFe = frame.peek(-1);
    RegisterID protoReg = frame.ownRegForData(protoFe);

    // Now, get the type. If it's not an object, set protoReg to NULL.
    JS_ASSERT_IF(protoFe->isTypeKnown(), protoFe->isType(JSVAL_TYPE_OBJECT));
    if (!protoFe->isType(JSVAL_TYPE_OBJECT)) {
        Jump isNotObject = frame.testObject(Assembler::NotEqual, protoFe);
        stubcc.linkExitDirect(isNotObject, stubcc.masm.label());
        stubcc.masm.move(ImmPtr(NULL), protoReg);
        stubcc.crossJump(stubcc.masm.jump(), masm.label());
    }

    // Done with the protoFe.
    frame.pop();

    prepareStubCall(Uses(0));
    if (protoReg != Registers::ArgReg1)
        masm.move(protoReg, Registers::ArgReg1);
    INLINE_STUBCALL(stubs::CreateThis, REJOIN_RESUME);
    frame.freeReg(protoReg);
    return true;
}

bool
mjit::Compiler::jsop_tableswitch(jsbytecode *pc)
{
#if defined JS_CPU_ARM
    JS_NOT_REACHED("Implement jump(BaseIndex) for ARM");
    return true;
#else
    jsbytecode *originalPC = pc;
    DebugOnly<JSOp> op = JSOp(*originalPC);
    JS_ASSERT(op == JSOP_TABLESWITCH);

    uint32_t defaultTarget = GET_JUMP_OFFSET(pc);
    pc += JUMP_OFFSET_LEN;

    int32_t low = GET_JUMP_OFFSET(pc);
    pc += JUMP_OFFSET_LEN;
    int32_t high = GET_JUMP_OFFSET(pc);
    pc += JUMP_OFFSET_LEN;
    int numJumps = high + 1 - low;
    JS_ASSERT(numJumps >= 0);

    FrameEntry *fe = frame.peek(-1);
    if (fe->isNotType(JSVAL_TYPE_INT32) || numJumps > 256) {
        frame.syncAndForgetEverything();
        masm.move(ImmPtr(originalPC), Registers::ArgReg1);

        /* prepareStubCall() is not needed due to forgetEverything() */
        INLINE_STUBCALL(stubs::TableSwitch, REJOIN_NONE);
        frame.pop();
        masm.jump(Registers::ReturnReg);
        return true;
    }

    RegisterID dataReg;
    if (fe->isConstant()) {
        JS_ASSERT(fe->isType(JSVAL_TYPE_INT32));
        dataReg = frame.allocReg();
        masm.move(Imm32(fe->getValue().toInt32()), dataReg);
    } else {
        dataReg = frame.copyDataIntoReg(fe);
    }

    RegisterID reg = frame.allocReg();
    frame.syncAndForgetEverything();

    MaybeJump notInt;
    if (!fe->isType(JSVAL_TYPE_INT32))
        notInt = masm.testInt32(Assembler::NotEqual, frame.addressOf(fe));

    JumpTable jt;
    jt.offsetIndex = jumpTableEdges.length();
    jt.label = masm.moveWithPatch(ImmPtr(NULL), reg);
    jumpTables.append(jt);

    for (int i = 0; i < numJumps; i++) {
        uint32_t target = GET_JUMP_OFFSET(pc);
        if (!target)
            target = defaultTarget;
        JumpTableEdge edge;
        edge.source = originalPC - script->code;
        edge.target = (originalPC + target) - script->code;
        jumpTableEdges.append(edge);
        pc += JUMP_OFFSET_LEN;
    }
    if (low != 0)
        masm.sub32(Imm32(low), dataReg);
    Jump defaultCase = masm.branch32(Assembler::AboveOrEqual, dataReg, Imm32(numJumps));
    BaseIndex jumpTarget(reg, dataReg, Assembler::ScalePtr);
    masm.jump(jumpTarget);

    if (notInt.isSet()) {
        stubcc.linkExitDirect(notInt.get(), stubcc.masm.label());
        stubcc.leave();
        stubcc.masm.move(ImmPtr(originalPC), Registers::ArgReg1);
        OOL_STUBCALL(stubs::TableSwitch, REJOIN_NONE);
        stubcc.masm.jump(Registers::ReturnReg);
    }
    frame.pop();
    return jumpAndRun(defaultCase, originalPC + defaultTarget);
#endif
}

void
mjit::Compiler::jsop_toid()
{
    /* Leave integers alone, stub everything else. */
    FrameEntry *top = frame.peek(-1);

    if (top->isType(JSVAL_TYPE_INT32))
        return;

    if (top->isNotType(JSVAL_TYPE_INT32)) {
        prepareStubCall(Uses(2));
        INLINE_STUBCALL(stubs::ToId, REJOIN_FALLTHROUGH);
        frame.pop();
        pushSyncedEntry(0);
        return;
    }

    frame.syncAt(-1);

    Jump j = frame.testInt32(Assembler::NotEqual, top);
    stubcc.linkExit(j, Uses(2));

    stubcc.leave();
    OOL_STUBCALL(stubs::ToId, REJOIN_FALLTHROUGH);

    frame.pop();
    pushSyncedEntry(0);

    stubcc.rejoin(Changes(1));
}

void
mjit::Compiler::jsop_in()
{
    FrameEntry *obj = frame.peek(-1);
    FrameEntry *id = frame.peek(-2);

    if (cx->typeInferenceEnabled() && id->isType(JSVAL_TYPE_INT32)) {
        types::TypeSet *types = analysis->poppedTypes(PC, 0);

        if (obj->mightBeType(JSVAL_TYPE_OBJECT) &&
            !types->hasObjectFlags(cx, types::OBJECT_FLAG_NON_DENSE_ARRAY) &&
            !types::ArrayPrototypeHasIndexedProperty(cx, outerScript))
        {
            bool isPacked = !types->hasObjectFlags(cx, types::OBJECT_FLAG_NON_PACKED_ARRAY);

            if (!obj->isTypeKnown()) {
                Jump guard = frame.testObject(Assembler::NotEqual, obj);
                stubcc.linkExit(guard, Uses(2));
            }

            RegisterID dataReg = frame.copyDataIntoReg(obj);

            Int32Key key = id->isConstant()
                         ? Int32Key::FromConstant(id->getValue().toInt32())
                         : Int32Key::FromRegister(frame.tempRegForData(id));

            masm.loadPtr(Address(dataReg, JSObject::offsetOfElements()), dataReg);

            // Guard on the array's initialized length.
            Jump initlenGuard = masm.guardArrayExtent(ObjectElements::offsetOfInitializedLength(),
                                                      dataReg, key, Assembler::BelowOrEqual);

            // Guard to make sure we don't have a hole. Skip it if the array is packed.
            MaybeJump holeCheck;
            if (!isPacked)
                holeCheck = masm.guardElementNotHole(dataReg, key);

            masm.move(Imm32(1), dataReg);
            Jump done = masm.jump();

            Label falseBranch = masm.label();
            initlenGuard.linkTo(falseBranch, &masm);
            if (!isPacked)
                holeCheck.getJump().linkTo(falseBranch, &masm);
            masm.move(Imm32(0), dataReg);

            done.linkTo(masm.label(), &masm);

            stubcc.leave();
            OOL_STUBCALL_USES(stubs::In, REJOIN_PUSH_BOOLEAN, Uses(2));

            frame.popn(2);
            if (dataReg != Registers::ReturnReg)
                stubcc.masm.move(Registers::ReturnReg, dataReg);

            frame.pushTypedPayload(JSVAL_TYPE_BOOLEAN, dataReg);

            stubcc.rejoin(Changes(2));

            return;
        }
    }

    prepareStubCall(Uses(2));
    INLINE_STUBCALL(stubs::In, REJOIN_PUSH_BOOLEAN);
    frame.popn(2);
    frame.takeReg(Registers::ReturnReg);
    frame.pushTypedPayload(JSVAL_TYPE_BOOLEAN, Registers::ReturnReg);
}

/*
 * For any locals or args which we know to be integers but are treated as
 * doubles by the type inference, convert to double. These will be assumed to be
 * doubles at control flow join points. This function must be called before
 * branching to another opcode.
 *
 * We can only carry entries as doubles when we can track all incoming edges to
 * a join point (no try blocks etc.) and when we can track all writes to the
 * local/arg (the slot does not escape) and ensure the Compiler representation
 * matches the inferred type for the variable's SSA value. These properties are
 * both ensured by analysis->trackSlot.
 */
void
mjit::Compiler::fixDoubleTypes(jsbytecode *target)
{
    if (!cx->typeInferenceEnabled())
        return;

    /*
     * Fill fixedIntToDoubleEntries with all variables that are known to be an
     * int here and a double at the branch target, and fixedDoubleToAnyEntries
     * with all variables that are known to be a double here but not at the
     * branch target.
     *
     * Per prepareInferenceTypes, the target state consists of the current
     * state plus any phi nodes or other new values introduced at the target.
     */
    JS_ASSERT(fixedIntToDoubleEntries.empty());
    JS_ASSERT(fixedDoubleToAnyEntries.empty());
    const SlotValue *newv = analysis->newValues(target);
    if (newv) {
        while (newv->slot) {
            if (newv->value.kind() != SSAValue::PHI ||
                newv->value.phiOffset() != uint32_t(target - script->code) ||
                !analysis->trackSlot(newv->slot)) {
                newv++;
                continue;
            }
            JS_ASSERT(newv->slot < TotalSlots(script));
            types::TypeSet *targetTypes = analysis->getValueTypes(newv->value);
            FrameEntry *fe = frame.getSlotEntry(newv->slot);
            VarType &vt = a->varTypes[newv->slot];
            JSValueType type = vt.getTypeTag(cx);
            if (targetTypes->getKnownTypeTag(cx) == JSVAL_TYPE_DOUBLE) {
                if (type == JSVAL_TYPE_INT32) {
                    fixedIntToDoubleEntries.append(newv->slot);
                    frame.ensureDouble(fe);
                    frame.forgetLoopReg(fe);
                } else if (type == JSVAL_TYPE_UNKNOWN) {
                    /*
                     * Unknown here but a double at the target. The type
                     * set for the existing value must be empty, so this
                     * code is doomed and we can just mark the value as
                     * a double.
                     */
                    frame.ensureDouble(fe);
                } else {
                    JS_ASSERT(type == JSVAL_TYPE_DOUBLE);
                }
            } else if (type == JSVAL_TYPE_DOUBLE) {
                fixedDoubleToAnyEntries.append(newv->slot);
                frame.syncAndForgetFe(fe);
                frame.forgetLoopReg(fe);
            }
            newv++;
        }
    }
}

void
mjit::Compiler::watchGlobalReallocation()
{
    JS_ASSERT(cx->typeInferenceEnabled());
    if (hasGlobalReallocation)
        return;
    types::TypeSet::WatchObjectStateChange(cx, globalObj->getType(cx));
    hasGlobalReallocation = true;
}

void
mjit::Compiler::updateVarType()
{
    if (!cx->typeInferenceEnabled())
        return;

    /*
     * For any non-escaping variable written at the current opcode, update the
     * associated type sets according to the written type, keeping the type set
     * for each variable in sync with what the SSA analysis has determined
     * (see prepareInferenceTypes).
     */

    types::TypeSet *types = pushedTypeSet(0);
    uint32_t slot = GetBytecodeSlot(script, PC);

    if (analysis->trackSlot(slot)) {
        VarType &vt = a->varTypes[slot];
        vt.setTypes(types);

        /*
         * Variables whose type has been inferred as a double need to be
         * maintained by the frame as a double. We might forget the exact
         * representation used by the next call to fixDoubleTypes, fix it now.
         */
        if (vt.getTypeTag(cx) == JSVAL_TYPE_DOUBLE)
            frame.ensureDouble(frame.getSlotEntry(slot));
    }
}

void
mjit::Compiler::updateJoinVarTypes()
{
    if (!cx->typeInferenceEnabled())
        return;

    /* Update variable types for all new values at this bytecode. */
    const SlotValue *newv = analysis->newValues(PC);
    if (newv) {
        while (newv->slot) {
            if (newv->slot < TotalSlots(script)) {
                VarType &vt = a->varTypes[newv->slot];
                JSValueType type = vt.getTypeTag(cx);
                vt.setTypes(analysis->getValueTypes(newv->value));
                if (vt.getTypeTag(cx) != type) {
                    /*
                     * If the known type of a variable changes (even if the
                     * variable itself has not been reassigned) then we can't
                     * carry a loop register for the var.
                     */
                    FrameEntry *fe = frame.getSlotEntry(newv->slot);
                    frame.forgetLoopReg(fe);
                }
            }
            newv++;
        }
    }
}

void
mjit::Compiler::restoreVarType()
{
    if (!cx->typeInferenceEnabled())
        return;

    uint32_t slot = GetBytecodeSlot(script, PC);

    if (slot >= analyze::TotalSlots(script))
        return;

    /*
     * Restore the known type of a live local or argument. We ensure that types
     * of tracked variables match their inferred type (as tracked in varTypes),
     * but may have forgotten it due to a branch or syncAndForgetEverything.
     */
    JSValueType type = a->varTypes[slot].getTypeTag(cx);
    if (type != JSVAL_TYPE_UNKNOWN &&
        (type != JSVAL_TYPE_DOUBLE || analysis->trackSlot(slot))) {
        FrameEntry *fe = frame.getSlotEntry(slot);
        JS_ASSERT_IF(fe->isTypeKnown(), fe->isType(type));
        if (!fe->isTypeKnown())
            frame.learnType(fe, type, false);
    }
}

JSValueType
mjit::Compiler::knownPushedType(uint32_t pushed)
{
    if (!cx->typeInferenceEnabled())
        return JSVAL_TYPE_UNKNOWN;
    types::TypeSet *types = analysis->pushedTypes(PC, pushed);
    return types->getKnownTypeTag(cx);
}

bool
mjit::Compiler::mayPushUndefined(uint32_t pushed)
{
    JS_ASSERT(cx->typeInferenceEnabled());

    /*
     * This should only be used when the compiler is checking if it is OK to push
     * undefined without going to a stub that can trigger recompilation.
     * If this returns false and undefined subsequently becomes a feasible
     * value pushed by the bytecode, recompilation will *NOT* be triggered.
     */
    types::TypeSet *types = analysis->pushedTypes(PC, pushed);
    return types->hasType(types::Type::UndefinedType());
}

types::TypeSet *
mjit::Compiler::pushedTypeSet(uint32_t pushed)
{
    if (!cx->typeInferenceEnabled())
        return NULL;
    return analysis->pushedTypes(PC, pushed);
}

bool
mjit::Compiler::monitored(jsbytecode *pc)
{
    if (!cx->typeInferenceEnabled())
        return false;
    return analysis->getCode(pc).monitoredTypes;
}

bool
mjit::Compiler::hasTypeBarriers(jsbytecode *pc)
{
    if (!cx->typeInferenceEnabled())
        return false;

    return analysis->typeBarriers(cx, pc) != NULL;
}

void
mjit::Compiler::pushSyncedEntry(uint32_t pushed)
{
    frame.pushSynced(knownPushedType(pushed));
}

JSObject *
mjit::Compiler::pushedSingleton(unsigned pushed)
{
    if (!cx->typeInferenceEnabled())
        return NULL;

    types::TypeSet *types = analysis->pushedTypes(PC, pushed);
    return types->getSingleton(cx);
}

/*
 * Barriers overview.
 *
 * After a property fetch finishes, we may need to do type checks on it to make
 * sure it matches the pushed type set for this bytecode. This can be either
 * because there is a type barrier at the bytecode, or because we cannot rule
 * out an undefined result. For such accesses, we push a register pair, and
 * then use those registers to check the fetched type matches the inferred
 * types for the pushed set. The flow here is tricky:
 *
 * frame.pushRegs(type, data, knownType);
 * --- Depending on knownType, the frame's representation for the pushed entry
 *     may not be a register pair anymore. knownType is based on the observed
 *     types that have been pushed here and may not actually match type/data.
 *     pushRegs must not clobber either register, for the test below.
 *
 * testBarrier(type, data)
 * --- Use the type/data regs and generate a single jump taken if the barrier
 *     has been violated.
 *
 * --- Rearrange stack, rejoin from stub paths. No code must be emitted into
 *     the inline path between testBarrier and finishBarrier. Since a stub path
 *     may be in progress we can't call finishBarrier before stubcc.rejoin,
 *     and since typeReg/dataReg may not be intact after the stub call rejoin
 *     (if knownType != JSVAL_TYPE_UNKNOWN) we can't testBarrier after calling
 *     stubcc.rejoin.
 *
 * finishBarrier()
 * --- Link the barrier jump to a new stub code path which updates the pushed
 *     types (possibly triggering recompilation). The frame has changed since
 *     pushRegs to reflect the final state of the op, which is OK as no inline
 *     code has been emitted since the barrier jump.
 */

mjit::Compiler::BarrierState
mjit::Compiler::pushAddressMaybeBarrier(Address address, JSValueType type, bool reuseBase,
                                        bool testUndefined)
{
    if (!hasTypeBarriers(PC) && !testUndefined) {
        frame.push(address, type, reuseBase);
        return BarrierState();
    }

    RegisterID typeReg, dataReg;
    frame.loadIntoRegisters(address, reuseBase, &typeReg, &dataReg);

    frame.pushRegs(typeReg, dataReg, type);
    return testBarrier(typeReg, dataReg, testUndefined);
}

MaybeJump
mjit::Compiler::trySingleTypeTest(types::TypeSet *types, RegisterID typeReg)
{
    /*
     * If a type set we have a barrier on is monomorphic, generate a single
     * jump taken if a type register has a match. This doesn't handle type sets
     * containing objects, as these require two jumps regardless (test for
     * object, then test the type of the object).
     */
    MaybeJump res;

    switch (types->getKnownTypeTag(cx)) {
      case JSVAL_TYPE_INT32:
        res.setJump(masm.testInt32(Assembler::NotEqual, typeReg));
        return res;

      case JSVAL_TYPE_DOUBLE:
        res.setJump(masm.testNumber(Assembler::NotEqual, typeReg));
        return res;

      case JSVAL_TYPE_BOOLEAN:
        res.setJump(masm.testBoolean(Assembler::NotEqual, typeReg));
        return res;

      case JSVAL_TYPE_STRING:
        res.setJump(masm.testString(Assembler::NotEqual, typeReg));
        return res;

      default:
        return res;
    }
}

JSC::MacroAssembler::Jump
mjit::Compiler::addTypeTest(types::TypeSet *types, RegisterID typeReg, RegisterID dataReg)
{
    /*
     * :TODO: It would be good to merge this with GenerateTypeCheck, but the
     * two methods have a different format for the tested value (in registers
     * vs. in memory).
     */

    Vector<Jump> matches(CompilerAllocPolicy(cx, *this));

    if (types->hasType(types::Type::Int32Type()))
        matches.append(masm.testInt32(Assembler::Equal, typeReg));

    if (types->hasType(types::Type::DoubleType()))
        matches.append(masm.testDouble(Assembler::Equal, typeReg));

    if (types->hasType(types::Type::UndefinedType()))
        matches.append(masm.testUndefined(Assembler::Equal, typeReg));

    if (types->hasType(types::Type::BooleanType()))
        matches.append(masm.testBoolean(Assembler::Equal, typeReg));

    if (types->hasType(types::Type::StringType()))
        matches.append(masm.testString(Assembler::Equal, typeReg));

    if (types->hasType(types::Type::NullType()))
        matches.append(masm.testNull(Assembler::Equal, typeReg));

    unsigned count = 0;
    if (types->hasType(types::Type::AnyObjectType()))
        matches.append(masm.testObject(Assembler::Equal, typeReg));
    else
        count = types->getObjectCount();

    if (count != 0) {
        Jump notObject = masm.testObject(Assembler::NotEqual, typeReg);
        Address typeAddress(dataReg, JSObject::offsetOfType());

        for (unsigned i = 0; i < count; i++) {
            if (JSObject *object = types->getSingleObject(i))
                matches.append(masm.branchPtr(Assembler::Equal, dataReg, ImmPtr(object)));
        }

        for (unsigned i = 0; i < count; i++) {
            if (types::TypeObject *object = types->getTypeObject(i))
                matches.append(masm.branchPtr(Assembler::Equal, typeAddress, ImmPtr(object)));
        }

        notObject.linkTo(masm.label(), &masm);
    }

    Jump mismatch = masm.jump();

    for (unsigned i = 0; i < matches.length(); i++)
        matches[i].linkTo(masm.label(), &masm);

    return mismatch;
}

mjit::Compiler::BarrierState
mjit::Compiler::testBarrier(RegisterID typeReg, RegisterID dataReg,
                            bool testUndefined, bool testReturn, bool force)
{
    BarrierState state;
    state.typeReg = typeReg;
    state.dataReg = dataReg;

    if (!cx->typeInferenceEnabled() || !(js_CodeSpec[*PC].format & JOF_TYPESET))
        return state;

    types::TypeSet *types = analysis->bytecodeTypes(PC);
    if (types->unknown()) {
        /*
         * If the result of this opcode is already unknown, there is no way for
         * a type barrier to fail.
         */
        return state;
    }

    if (testReturn) {
        JS_ASSERT(!testUndefined);
        if (!analysis->getCode(PC).monitoredTypesReturn)
            return state;
    } else if (!hasTypeBarriers(PC) && !force) {
        if (testUndefined && !types->hasType(types::Type::UndefinedType()))
            state.jump.setJump(masm.testUndefined(Assembler::Equal, typeReg));
        return state;
    }

    types->addFreeze(cx);

    /* Cannot have type barriers when the result of the operation is already unknown. */
    JS_ASSERT(!types->unknown());

    state.jump = trySingleTypeTest(types, typeReg);
    if (!state.jump.isSet())
        state.jump.setJump(addTypeTest(types, typeReg, dataReg));

    return state;
}

void
mjit::Compiler::finishBarrier(const BarrierState &barrier, RejoinState rejoin, uint32_t which)
{
    if (!barrier.jump.isSet())
        return;

    stubcc.linkExitDirect(barrier.jump.get(), stubcc.masm.label());

    /*
     * Before syncing, store the entry to sp[0]. (scanInlineCalls accounted for
     * this when making sure there is enough froom for all frames). The known
     * type in the frame may be wrong leading to an incorrect sync, and this
     * sync may also clobber typeReg and/or dataReg.
     */
    frame.pushSynced(JSVAL_TYPE_UNKNOWN);
    stubcc.masm.storeValueFromComponents(barrier.typeReg, barrier.dataReg,
                                         frame.addressOf(frame.peek(-1)));
    frame.pop();

    stubcc.syncExit(Uses(0));
    stubcc.leave();

    stubcc.masm.move(ImmIntPtr(intptr_t(which)), Registers::ArgReg1);
    OOL_STUBCALL(stubs::TypeBarrierHelper, rejoin);
    stubcc.rejoin(Changes(0));
}

void
mjit::Compiler::testPushedType(RejoinState rejoin, int which, bool ool)
{
    if (!cx->typeInferenceEnabled() || !(js_CodeSpec[*PC].format & JOF_TYPESET))
        return;

    types::TypeSet *types = analysis->bytecodeTypes(PC);
    if (types->unknown())
        return;

    Assembler &masm = ool ? stubcc.masm : this->masm;

    JS_ASSERT(which <= 0);
    Address address = (which == 0) ? frame.addressOfTop() : frame.addressOf(frame.peek(which));

    Vector<Jump> mismatches(cx);
    if (!masm.generateTypeCheck(cx, address, types, &mismatches)) {
        oomInVector = true;
        return;
    }

    Jump j = masm.jump();

    for (unsigned i = 0; i < mismatches.length(); i++)
        mismatches[i].linkTo(masm.label(), &masm);

    masm.move(Imm32(which), Registers::ArgReg1);
    if (ool)
        OOL_STUBCALL(stubs::StubTypeHelper, rejoin);
    else
        INLINE_STUBCALL(stubs::StubTypeHelper, rejoin);

    j.linkTo(masm.label(), &masm);
}
