cc_test_old(
  ir_kernel_dialect_pass_test
  SRCS
  ir_kernel_dialect_pass_test.cc
  DEPS
  pd_op_to_kernel_pass
  pd_dialect
  phi_kernel_adaptor
  pd_trait
  ir
  phi
  gtest)
