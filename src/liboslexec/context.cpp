// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <OpenImageIO/sysutil.h>
#include <OpenImageIO/thread.h>
#include <OpenImageIO/timer.h>

#include <OSL/oslconfig.h>

#if OSL_USE_BATCHED
#    include <OSL/batched_shaderglobals.h>
#endif

#include <OSL/mask.h>
#include <OSL/wide.h>

#include "oslexec_pvt.h"

OSL_NAMESPACE_BEGIN

static mutex buffered_errors_mutex;
#if OSL_USE_BATCHED
static mutex buffered_file_output_mutex;
#endif


namespace pvt {

template<size_t ByteAlignmentT, typename T>
inline bool
is_aligned(const T* pointer)
{
    std::uintptr_t ptrAsUint = reinterpret_cast<std::uintptr_t>(pointer);
    return (ptrAsUint % ByteAlignmentT == 0);
}

}  // namespace pvt

ShadingContext::ShadingContext(ShadingSystemImpl& shadingsys,
                               PerThreadInfo* threadinfo)
    : m_shadingsys(shadingsys)
    , m_renderer(m_shadingsys.renderer())
    , m_threadinfo(threadinfo)
    , m_group(NULL)
    , m_max_warnings(shadingsys.max_warnings_per_thread())
    , m_dictionary(NULL)
    , batch_size_executed(0)
{
    m_shadingsys.m_stat_contexts += 1;
    m_texture_thread_info = NULL;
}



ShadingContext::~ShadingContext()
{
    process_errors();
#if OSL_USE_BATCHED
    process_file_output();
#endif
    m_shadingsys.m_stat_contexts -= 1;
    free_dict_resources();
}

ShadingContext::RestoreState
ShadingContext::repurposeForJit()
{
    process_errors();
    // Match previous behavior of nullptr value for texture thread info
    // as if we created a new ShadingContext
    RestoreState restore_state { texture_thread_info() };
    texture_thread_info(nullptr);
    return restore_state;
}

// Process any errors from JIT and restore the texture_thread_info
void
ShadingContext::restoreFromJit(const RestoreState& restore_state)
{
    // Restore "this" ShadingContext for execution
    process_errors();
    texture_thread_info(restore_state.m_pre_jit_texture_thread_info);
}


bool
ShadingContext::execute_init(ShaderGroup& sgroup, int threadindex,
                             int shadeindex, ShaderGlobals& ssg,
                             void* userdata_base_ptr, void* output_base_ptr,
                             bool run)
{
    if (m_group)
        execute_cleanup();
    batch_size_executed = 0;
    m_group             = &sgroup;
    m_ticks             = 0;

    // Optimize if we haven't already
    if (sgroup.nlayers()) {
        sgroup.start_running();
        if (!sgroup.jitted()) {
            auto restore_state = repurposeForJit();
            shadingsys().optimize_group(sgroup, this, true /*do_jit*/);
            if (shadingsys().m_greedyjit
                && shadingsys().m_groups_to_compile_count) {
                // If we are greedily JITing, optimize/JIT everything now
                shadingsys().optimize_all_groups();
            }
            restoreFromJit(restore_state);
        }
        if (sgroup.does_nothing())
            return false;
    } else {
        // empty shader - nothing to do!
        return false;
    }

    int profile = shadingsys().m_profile;
    OIIO::Timer timer(profile ? OIIO::Timer::StartNow
                              : OIIO::Timer::DontStartNow);

    // Allocate enough space on the heap
    size_t heap_size_needed = sgroup.llvm_groupdata_size();
    reserve_heap(heap_size_needed);
    // Zero out the heap memory we will be using
    if (shadingsys().m_clearmemory)
        memset(m_heap.get(), 0, heap_size_needed);

    // Set up closure storage
    m_closure_pool.clear();

    // Clear the message blackboard
    m_messages.clear();

    // Clear miscellaneous scratch space
    m_scratch_pool.clear();

    // Zero out stats for this execution
    clear_runtime_stats();

    if (run) {
        RunLLVMGroupFunc run_func = sgroup.llvm_compiled_init();
        if (!run_func)
            return false;
        ssg.context             = this;
        ssg.shadingStateUniform = &(shadingsys().m_shading_state_uniform);
        ssg.renderer            = renderer();
        ssg.Ci                  = NULL;
        ssg.thread_index        = threadindex;
        ssg.shade_index         = shadeindex;
        //TODO: Possible remove shadeindex from run_func
        run_func(&ssg, m_heap.get(), userdata_base_ptr, output_base_ptr,
                 shadeindex, sgroup.interactive_arena_ptr());
    }

    if (profile)
        m_ticks += timer.ticks();
    return true;
}



bool
ShadingContext::execute_layer(int threadindex, int shadeindex,
                              ShaderGlobals& ssg, void* userdata_base_ptr,
                              void* output_base_ptr, int layernumber)
{
    if (!group() || group()->nlayers() == 0 || group()->does_nothing())
        return false;
    OSL_DASSERT(ssg.context == this && ssg.renderer == renderer());

    int profile = shadingsys().m_profile;
    OIIO::Timer timer(profile ? OIIO::Timer::StartNow
                              : OIIO::Timer::DontStartNow);

    RunLLVMGroupFunc run_func = group()->llvm_compiled_layer(layernumber);
    if (!run_func)
        return false;

    run_func(&ssg, m_heap.get(), userdata_base_ptr, output_base_ptr, shadeindex,
             group()->interactive_arena_ptr());

    if (profile)
        m_ticks += timer.ticks();

    return true;
}



bool
ShadingContext::execute_cleanup()
{
    if (!group()) {
        errorfmt("execute_cleanup called again on a cleaned-up context");
        return false;
    }

    // Process any queued up error messages, warnings, printfs from shaders
    process_errors();
#if OSL_USE_BATCHED
    process_file_output();
#endif

    if (shadingsys().m_profile) {
        record_runtime_stats();  // Transfer runtime stats to the shadingsys
        shadingsys().m_stat_total_shading_time_ticks += m_ticks;
        group()->m_stat_total_shading_time_ticks += m_ticks;
    }

    return true;
}



bool
ShadingContext::execute(ShaderGroup& sgroup, int threadindex, int shadeindex,
                        ShaderGlobals& ssg, void* userdata_base_ptr,
                        void* output_base_ptr, bool run)
{
    int n = sgroup.m_exec_repeat;
    Vec3 Psave, Nsave;  // for repeats
    bool repeat = (n > 1);
    if (repeat) {
        // If we're going to repeat more than once, we need to save any
        // globals that might get modified.
        Psave = ssg.P;
        Nsave = ssg.N;
        if (!run)
            n = 1;
    }

    bool result = true;
    while (1) {
        if (!execute_init(sgroup, threadindex, shadeindex, ssg,
                          userdata_base_ptr, output_base_ptr, run))
            return false;
        if (run && n)
            execute_layer(threadindex, shadeindex, ssg, userdata_base_ptr,
                          output_base_ptr, group()->nlayers() - 1);
        result = execute_cleanup();
        if (--n < 1)
            break;  // done
        if (repeat) {
            // Going around for another pass... restore things as best as we
            // can.
            ssg.P  = Psave;
            ssg.N  = Nsave;
            ssg.Ci = NULL;
        }
    }
    return result;
}


#if OSL_USE_BATCHED

template<int WidthT>
bool
ShadingContext::Batched<WidthT>::execute_init(
    ShaderGroup& sgroup, int batch_size,
    Wide<const int, WidthT> wide_shadeindex, BatchedShaderGlobals<WidthT>& bsg,
    void* userdata_base_ptr, void* output_base_ptr, bool run)
{
    if (context().m_group)
        context().execute_cleanup();

    context().batch_size_executed = batch_size;
    context().m_group             = &sgroup;
    context().m_ticks             = 0;

    // Optimize if we haven't already
    if (sgroup.nlayers()) {
        sgroup.start_running();
        if (!sgroup.batch_jitted()) {
            auto restore_state = context().repurposeForJit();
            shadingsys().template batched<WidthT>().jit_group(sgroup,
                                                              &context());

            if (shadingsys().m_greedyjit
                && shadingsys().m_groups_to_compile_count) {
                // If we are greedily JITing, optimize/JIT everything now
                shadingsys().template batched<WidthT>().jit_all_groups();
            }
            context().restoreFromJit(restore_state);
        }
        // To handle layers that were not used but still possibly had
        // render outputs, we always generate a run function even for
        // do nothing groups, so that a GroupData on the heap gets built
        // and the run function can broadcast default values there.
        //
        // Observation is that nothing ever overwrites that default value
        // so we could just run it once, or deal with broadcasting the
        // default value ourselves

    } else {
        // empty shader - nothing to do!
        return false;
    }

    int profile = shadingsys().m_profile;
    OIIO::Timer timer(profile ? OIIO::Timer::StartNow
                              : OIIO::Timer::DontStartNow);

    // Allocate enough space on the heap
    size_t heap_size_needed = sgroup.llvm_groupdata_wide_size();
    context().reserve_heap(heap_size_needed);
    // Zero out the heap memory we will be using
    if (shadingsys().m_clearmemory)
        memset(context().m_heap.get(), 0, heap_size_needed);

    // Set up closure storage
    context().m_closure_pool.clear();

    // Clear the message blackboard
    context().m_messages.clear();
    context().batched_messages_buffer().clear();

    // Clear miscellaneous scratch space
    context().m_scratch_pool.clear();

    // Zero out stats for this execution
    context().clear_runtime_stats();

    if (run) {
        bsg.uniform.context  = &context();
        bsg.uniform.renderer = context().renderer();
        assign_all(bsg.varying.Ci, (ClosureColor*)nullptr);
        RunLLVMGroupFuncWide run_func = sgroup.llvm_compiled_wide_init();
        OSL_DASSERT(run_func);
        OSL_DASSERT(sgroup.llvm_groupdata_wide_size() <= context().m_heapsize);

        if (batch_size > 0) {
            Mask<WidthT> run_mask(false);
            run_mask.set_count_on(batch_size);

            run_func(&bsg, context().m_heap.get(), &wide_shadeindex.data(),
                     userdata_base_ptr, output_base_ptr, run_mask.value(),
                     sgroup.interactive_arena_ptr());
        }
    }

    if (profile)
        context().m_ticks += timer.ticks();
    return true;
}

template<int WidthT>
bool
ShadingContext::Batched<WidthT>::execute_layer(
    int batch_size, Wide<const int, WidthT> wide_shadeindex,
    BatchedShaderGlobals<WidthT>& bsg, void* userdata_base_ptr,
    void* output_base_ptr, int layernumber)
{
    if (!group() || group()->nlayers() == 0 || group()->does_nothing()
        || (context().batch_size_executed != batch_size))
        return false;
    OSL_DASSERT(bsg.uniform.context == &context()
                && bsg.uniform.renderer == context().renderer());

    int profile = shadingsys().m_profile;
    OIIO::Timer timer(profile ? OIIO::Timer::StartNow
                              : OIIO::Timer::DontStartNow);

    RunLLVMGroupFuncWide run_func = group()->llvm_compiled_wide_layer(
        layernumber);
    if (!run_func)
        return false;

    OSL_ASSERT(pvt::is_aligned<64>(&bsg));
    OSL_ASSERT(pvt::is_aligned<64>(context().m_heap.get()));

    if (batch_size > 0) {
        Mask<WidthT> run_mask(false);
        run_mask.set_count_on(batch_size);

        run_func(&bsg, context().m_heap.get(), &wide_shadeindex.data(),
                 userdata_base_ptr, output_base_ptr, run_mask.value(),
                 group()->interactive_arena_ptr());
    }

    if (profile)
        context().m_ticks += timer.ticks();

    return true;
}


template<int WidthT>
bool
ShadingContext::Batched<WidthT>::execute(
    ShaderGroup& sgroup, int batch_size,
    Wide<const int, WidthT> wide_shadeindex, BatchedShaderGlobals<WidthT>& bsg,
    void* userdata_base_ptr, void* output_base_ptr, bool run)
{
    OSL_ASSERT(is_aligned<64>(&bsg));
    int n = sgroup.m_exec_repeat;

    Block<Vec3, WidthT> Psave, Nsave;  // for repeats
    bool repeat = (n > 1);
    if (repeat) {
        // If we're going to repeat more than once, we need to save any
        // globals that might get modified.
        Psave = bsg.varying.P;
        Nsave = bsg.varying.N;
        if (!run)
            n = 1;
    }

    bool result = true;
    while (1) {
        if (!execute_init(sgroup, batch_size, wide_shadeindex, bsg,
                          userdata_base_ptr, output_base_ptr, run))
            return false;
        if (run && n)
            execute_layer(batch_size, wide_shadeindex, bsg, userdata_base_ptr,
                          output_base_ptr, group()->nlayers() - 1);
        result = context().execute_cleanup();
        if (--n < 1)
            break;  // done
        if (repeat) {
            // Going around for another pass... restore things as best as we
            // can.
            bsg.varying.P = Psave;
            bsg.varying.N = Nsave;
            assign_all(bsg.varying.Ci, (ClosureColor*)nullptr);
        }
    }
    return result;
}

void
ShadingContext::record_error(ErrorHandler::ErrCode code,
                             const std::string& text,
                             Mask<MaxSupportedSimdLaneCount> mask) const
{
    m_buffered_errors.emplace_back(code, text, mask);
    // If we aren't buffering, just process immediately
    if (!shadingsys().m_buffer_printf)
        process_errors();
}

void
ShadingContext::record_to_file(ustring filename, const std::string& text,
                               Mask<MaxSupportedSimdLaneCount> mask) const
{
    m_buffered_file_output.emplace_back(filename, text, mask);
}

#endif

void
ShadingContext::record_error(ErrorHandler::ErrCode code,
                             const std::string& text) const
{
    m_buffered_errors.emplace_back(code, text);
    // If we aren't buffering, just process immediately
    if (!shadingsys().m_buffer_printf)
        process_errors();
}


// separate declaration from definition of template function
// to ensure noinline is respected
template<typename ErrorsT, typename TestFunctorT>
static OSL_NOINLINE void
process_errors_helper(ShadingSystemImpl& shading_sys, const ErrorsT& errors,
                      int startAtError, int endBeforeError,
                      const TestFunctorT& test_func);

// Given array of ErrorItems emit errors within the range startAtError to
// endBeforeError if and only if the test_func passed each ErrorItem's mask
// returns true.  This allows the same batch of errors to be processed for
// each data lane separately effectively serializing emission of errors,
// warnings, info, and messages
template<typename ErrorsT, typename TestFunctorT>
void
process_errors_helper(ShadingSystemImpl& shading_sys, const ErrorsT& errors,
                      int startAtError, int endBeforeError,
                      const TestFunctorT& test_func)
{
    for (int i = startAtError; i < endBeforeError; ++i) {
        const auto& error_item = errors[i];
        if (test_func(error_item.mask)) {
            switch (errors[i].err_code) {
            case ErrorHandler::EH_MESSAGE:
            case ErrorHandler::EH_DEBUG:
                shading_sys.message(error_item.msgString);
                break;
            case ErrorHandler::EH_INFO:
                shading_sys.info(error_item.msgString);
                break;
            case ErrorHandler::EH_WARNING:
                shading_sys.warning(error_item.msgString);
                break;
            case ErrorHandler::EH_ERROR:
            case ErrorHandler::EH_SEVERE:
                shading_sys.error(error_item.msgString);
                break;
            default: break;
            }
        }
    }
}

void
ShadingContext::process_errors() const
{
    int nerrors(m_buffered_errors.size());
    if (!nerrors)
        return;

    // Use a mutex to make sure output from different threads stays
    // together, at least for one shader invocation, rather than being
    // interleaved with other threads.
    lock_guard lock(buffered_errors_mutex);

#if OSL_USE_BATCHED
    if (execution_is_batched()) {
        OSL_DASSERT(batch_size_executed <= MaxSupportedSimdLaneCount);
        // Process each data lane separately and in the correct order
        for (int lane = 0; lane < batch_size_executed; ++lane) {
            OSL_INTEL_PRAGMA(noinline)
            process_errors_helper(
                shadingsys(), m_buffered_errors, 0, nerrors,
                // Test Function returns true to process the ErrorItem
                [=](Mask<MaxSupportedSimdLaneCount> mask) -> bool {
                    return mask.is_on(lane);
                });
        }
    } else
#endif
    {
        // Non-batch errors: ignore the mask, just print them out once
        OSL_INTEL_PRAGMA(noinline)
        process_errors_helper(
            shadingsys(), m_buffered_errors, 0, nerrors,
            // Test Function returns true to process the ErrorItem
            [=](Mask<MaxSupportedSimdLaneCount> /*mask*/) -> bool {
                return true;
            });
    }
    m_buffered_errors.clear();
}

#if OSL_USE_BATCHED
void
ShadingContext::process_file_output() const
{
    int nerrors(m_buffered_file_output.size());
    if (!nerrors)
        return;

    // Use a mutex to make sure output from different threads stays
    // together, at least for one shader invocation, rather than being
    // interleaved with other threads.
    lock_guard lock(buffered_file_output_mutex);

    OSL_ASSERT(execution_is_batched());
    OSL_DASSERT(batch_size_executed <= MaxSupportedSimdLaneCount);
    // Process each data lane separately and in the correct order
    for (int lane = 0; lane < batch_size_executed; ++lane) {
        for (int i = 0; i < nerrors; ++i) {
            const auto& item = m_buffered_file_output[i];
            if (item.mask.is_on(lane)) {
                // TODO: if performance critical, one could keep a cache of
                // open files instead of closing and reopening
                FILE* file = fopen(item.filename.c_str(), "a");
                fputs(item.msgString.c_str(), file);
                fclose(file);
            }
        }
    }
    m_buffered_file_output.clear();
}
#endif

const Symbol*
ShadingContext::symbol(ustring layername, ustring symbolname) const
{
    return group()->find_symbol(layername, symbolname);
}



const void*
ShadingContext::symbol_data(const Symbol& sym) const
{
    const ShaderGroup& sgroup(*group());
#if OSL_USE_BATCHED
    if (execution_is_batched()) {
        if (!sgroup.batch_jitted())
            return NULL;  // can't retrieve symbol if we didn't optimize & batched jit

        if (sym.wide_dataoffset() >= 0
            && (int)m_heapsize > sym.wide_dataoffset()) {
            // lives on the heap
            return m_heap.get() + sym.wide_dataoffset();
        }
    } else
#endif
    {
        if (!sgroup.jitted())
            return NULL;  // can't retrieve symbol if we didn't optimize & jit

        if (sym.dataoffset() >= 0 && (int)m_heapsize > sym.dataoffset()) {
            // lives on the heap
            return m_heap.get() + sym.dataoffset();
            // TODO(arenas): OSL_ASSERT(sym.arena() == SymArena::Heap);
            // TODO(arenas):  ??   return sym.dataptr(m_heap.get());
        }
    }

    // doesn't live on the heap
    if ((sym.symtype() == SymTypeParam || sym.symtype() == SymTypeOutputParam)
        && (sym.valuesource() == Symbol::DefaultVal
            || sym.valuesource() == Symbol::InstanceVal)) {
        // TODO(arenas): OSL_ASSERT(sym.arena() == SymArena::Absolute);
        return sym.dataptr();
    }

    return NULL;  // not something we can retrieve
}



const std::regex&
ShadingContext::find_regex(ustring r)
{
    RegexMap::const_iterator found = m_regex_map.find(r);
    if (found != m_regex_map.end())
        return *found->second;
    // otherwise, it wasn't found, add it
    m_regex_map[r].reset(new std::regex(r.c_str()));
    m_shadingsys.m_stat_regexes += 1;
    // std::cerr << "Made new regex for " << r << "\n";
    return *m_regex_map[r];
}



bool
ShadingContext::osl_get_attribute(ShaderGlobals* sg, void* objdata,
                                  int dest_derivs, ustringhash obj_name,
                                  ustringhash attr_name, int array_lookup,
                                  int index, TypeDesc attr_type,
                                  void* attr_dest)
{
#if 0
    // Change the #if's below if you want to
    OIIO::Timer timer;
#endif
    bool ok;

    if (array_lookup)
        ok = renderer()->get_array_attribute(sg, dest_derivs, obj_name,
                                             attr_type, attr_name, index,
                                             attr_dest);
    else
        ok = renderer()->get_attribute(sg, dest_derivs, obj_name, attr_type,
                                       attr_name, attr_dest);

#if 0
    double time = timer();
    shadingsys().m_stat_getattribute_time += time;
    if (!ok)
        shadingsys().m_stat_getattribute_fail_time += time;
    shadingsys().m_stat_getattribute_calls += 1;
#endif
    //    std::cout << "getattribute! '" << obj_name << "' " << attr_name << ' ' << attr_type.c_str() << " ok=" << ok << ", objdata was " << objdata << "\n";
    return ok;
}



OSL_SHADEOP void
osl_incr_layers_executed(ShaderGlobals* sg)
{
    ShadingContext* ctx = (ShadingContext*)sg->context;
    ctx->incr_layers_executed();
}

#if OSL_USE_BATCHED
// Explicit template instantiation for supported batch sizes
template class ShadingContext::Batched<16>;
template class ShadingContext::Batched<8>;
template class ShadingContext::Batched<4>;
#endif


OSL_NAMESPACE_END
