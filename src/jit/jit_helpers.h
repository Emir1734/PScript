#pragma once

#include "jit/jit_exec_state.h"

#include <cstdint>

namespace pscript::jit {

extern "C" {

void jit_helper_load_const(JitExecState* state, std::uint32_t dest, std::uint32_t const_index);
void jit_helper_move_local_to_reg(JitExecState* state, std::uint32_t dest, std::uint32_t local_index);
void jit_helper_move_reg_to_local(JitExecState* state, std::uint32_t local_index, std::uint32_t src);
void jit_helper_load_local_integer_to_reg(JitExecState* state, std::uint32_t dest, std::uint32_t local_index);
void jit_helper_store_local_integer_from_reg(JitExecState* state, std::uint32_t local_index, std::uint32_t src);
void jit_helper_store_local_integer_fast(JitExecState* state, std::uint32_t local_index, std::uint32_t src_reg);
void flush_shadow_integer_locals(JitExecState* state, const std::int32_t* local_to_shadow, std::uint32_t local_count);
void hydrate_shadow_integer_locals(JitExecState* state, const std::int32_t* local_to_shadow, std::uint32_t local_count);
// Parameter locals may live only in argument registers when JIT entry skips interpreter prologue stores.
void hydrate_shadow_integer_locals_for_entry(JitExecState* state, const std::int32_t* local_to_shadow,
                                             std::uint32_t local_count, std::uint32_t arity);
void jit_helper_binary(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right,
                       std::uint32_t op);
void jit_helper_compare(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right,
                        std::uint32_t op);
void jit_helper_return(JitExecState* state, std::uint32_t src);
void jit_helper_jump_if_false(JitExecState* state, std::uint32_t cond_reg, std::uint32_t target_insn_index);
void jit_helper_jump(JitExecState* state, std::uint32_t target_insn_index);
void jit_helper_move_reg(JitExecState* state, std::uint32_t dest, std::uint32_t src);
void jit_helper_load_const_int(JitExecState* state, std::uint32_t dest, std::uint32_t low,
                               std::uint32_t high);
void jit_helper_guard_integer(JitExecState* state, std::uint32_t reg, std::uint32_t site_id);
void jit_helper_int_add(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right);
void jit_helper_int_sub(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right);
void jit_helper_int_mul(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right);
void jit_helper_int_mod(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right);
void jit_helper_int_div(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right);
void jit_helper_copy_integer_reg(JitExecState* state, std::uint32_t dest, std::uint32_t src);
void jit_helper_int_less(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right);
void jit_helper_int_equal(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right);
void jit_helper_string_equal(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right);
void jit_helper_string_not_equal(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right);
void jit_helper_string_concat(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right);
void jit_helper_float_add(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right);
void jit_helper_float_sub(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right);
void jit_helper_float_mul(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right);
void jit_helper_float_div(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right);
void jit_helper_float_less(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right);
void jit_helper_float_equal(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right);
void jit_helper_unary_not(JitExecState* state, std::uint32_t dest, std::uint32_t src);
void jit_helper_get_index(JitExecState* state, std::uint32_t dest, std::uint32_t object, std::uint32_t index);
void jit_helper_get_field(JitExecState* state, std::uint32_t dest, std::uint32_t object, std::uint32_t field_index);
void jit_helper_get_iter(JitExecState* state, std::uint32_t dest, std::uint32_t iterable);
void jit_helper_iter_next(JitExecState* state, std::uint32_t dest, std::uint32_t iterator, std::uint32_t exit_insn);
void jit_helper_load_function(JitExecState* state, std::uint32_t dest, std::uint32_t function_index);
void jit_helper_load_upvalue(JitExecState* state, std::uint32_t dest, std::uint32_t upvalue_index);
void jit_helper_load_closure_capture(JitExecState* state, std::uint32_t dest, std::uint32_t closure_reg,
                                     std::uint32_t capture_index);
void jit_helper_store_upvalue(JitExecState* state, std::uint32_t upvalue_index, std::uint32_t src_reg);
void jit_helper_make_closure(JitExecState* state, std::uint32_t dest, std::uint32_t function_index);
void jit_helper_materialize_closure_integer_capture(JitExecState* state, std::uint32_t dest,
                                                    std::uint32_t inner_function_index, std::uint32_t low,
                                                    std::uint32_t high);
void jit_helper_materialize_closure_two_integer_captures(JitExecState* state, std::uint32_t dest,
                                                         std::uint32_t inner_function_index, std::uint32_t cap0,
                                                         std::uint32_t cap1);
void jit_helper_call_value(JitExecState* state, std::uint32_t dest, std::uint32_t callee_reg, std::uint32_t argc,
                         std::uint32_t ic_index);
void jit_helper_call(JitExecState* state, std::uint32_t dest, std::uint32_t first_arg, std::uint32_t callee_index,
                     std::uint32_t ic_index);
void jit_helper_call_slow(JitExecState* state, std::uint32_t dest, std::uint32_t first_arg,
                          std::uint32_t callee_index, std::uint32_t ic_index);
void jit_helper_call_value_slow(JitExecState* state, std::uint32_t dest, std::uint32_t callee_reg, std::uint32_t argc,
                                std::uint32_t ic_index);
void jit_helper_flush_integer_shadow(JitExecState* state);
void jit_helper_hydrate_integer_shadow(JitExecState* state);
bool jit_call_value_mono_emit_guard(JitExecState* state, std::uint32_t callee_reg, std::uint32_t argc);
JitExecState* jit_nested_begin_mono_call(JitExecState* caller, std::uint32_t first_arg, std::uint32_t callee_index,
                                         std::uint32_t ic_index);
JitExecState* jit_nested_begin_mono_call_value(JitExecState* caller, std::uint32_t first_arg,
                                               std::uint32_t callee_reg, std::uint32_t ic_index);
JitExecState* jit_nested_begin_mono_call_value_after_ic_guard(JitExecState* caller, std::uint32_t first_arg,
                                                              std::uint32_t callee_reg, std::uint32_t ic_index);
bool jit_nested_complete_mono_call(JitExecState* caller, JitExecState* callee, std::uint32_t dest);
bool jit_nested_complete_mono_call_emit(JitExecState* caller, JitExecState* callee, std::uint32_t dest);
bool jit_nested_invoke_mono_call_value_emit(JitExecState* caller, std::uint32_t dest, std::uint32_t first_arg,
                                            std::uint32_t callee_reg, std::uint32_t ic_index);
void* jit_mono_entry_ptr_for_ic(JitExecState* caller, std::uint32_t ic_index, std::uint32_t callee_index);
void jit_helper_guard_value_kind(JitExecState* state, std::uint32_t reg, std::uint32_t kind, std::uint32_t site_id);
void jit_helper_guard_field_object(JitExecState* state, std::uint32_t reg, std::uint32_t site_id);
void jit_helper_list_get_index(JitExecState* state, std::uint32_t dest, std::uint32_t object, std::uint32_t index);
void jit_helper_tuple_get_index(JitExecState* state, std::uint32_t dest, std::uint32_t object, std::uint32_t index);
void jit_helper_bytes_get_index(JitExecState* state, std::uint32_t dest, std::uint32_t object, std::uint32_t index);
void jit_helper_dict_get_string_key(JitExecState* state, std::uint32_t dest, std::uint32_t object,
                                    std::uint32_t key_reg);
void jit_helper_list_get_int_element(JitExecState* state, std::uint32_t dest, std::uint32_t object,
                                     std::uint32_t index_reg);
void jit_helper_set_index(JitExecState* state, std::uint32_t object, std::uint32_t index, std::uint32_t value);
void jit_helper_list_set_index(JitExecState* state, std::uint32_t object, std::uint32_t index, std::uint32_t value);
void jit_helper_make_list(JitExecState* state, std::uint32_t dest, std::uint32_t first_reg, std::uint32_t count);
void jit_helper_make_dict(JitExecState* state, std::uint32_t dest, std::uint32_t first_pair_reg, std::uint32_t count);
void jit_helper_make_tuple(JitExecState* state, std::uint32_t dest, std::uint32_t first_reg, std::uint32_t count);
void jit_helper_make_set(JitExecState* state, std::uint32_t dest, std::uint32_t first_reg, std::uint32_t count);
void jit_helper_make_struct(JitExecState* state, std::uint32_t dest, std::uint32_t struct_index);
void jit_helper_set_field(JitExecState* state, std::uint32_t object, std::uint32_t field_index, std::uint32_t value);

}

}  // namespace pscript::jit
