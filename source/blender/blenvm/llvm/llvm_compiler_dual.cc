/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) Blender Foundation.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Lukas Toenne
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/blenvm/llvm/llvm_compiler_dual.cc
 *  \ingroup llvm
 */

#include <cstdio>
#include <set>
#include <sstream>

#include "node_graph.h"

#include "llvm_compiler.h"
#include "llvm_engine.h"
#include "llvm_function.h"
#include "llvm_headers.h"
#include "llvm_modules.h"
#include "llvm_types.h"

#include "util_opcode.h"

#include "modules.h"

namespace blenvm {

llvm::Module *LLVMTextureCompiler::m_nodes_module = NULL;

void LLVMTextureCompiler::node_graph_begin()
{
	
}

void LLVMTextureCompiler::node_graph_end()
{
	m_output_values.clear();
}

bool LLVMTextureCompiler::has_node_value(const ConstOutputKey &output) const
{
	return m_output_values.find(output) != m_output_values.end();
}

void LLVMTextureCompiler::alloc_node_value(llvm::BasicBlock *block, const ConstOutputKey &output)
{
	using namespace llvm;
	
	IRBuilder<> builder(context());
	builder.SetInsertPoint(block);
	
	const TypeSpec *typespec = output.socket->typedesc.get_typespec();
	Type *type = bvm_get_llvm_type(context(), typespec, false);
	BLI_assert(type != NULL);
	
	DualValue value(builder.CreateAlloca(type),
	                builder.CreateAlloca(type),
	                builder.CreateAlloca(type));
	
	/* use as node output values */
	bool ok = m_output_values.insert(OutputValueMap::value_type(output, value)).second;
	BLI_assert(ok && "Value for node output already defined!");
	UNUSED_VARS(ok);
}

void LLVMTextureCompiler::copy_node_value(const ConstOutputKey &from, const ConstOutputKey &to)
{
	using namespace llvm;
	
	DualValue value = m_output_values.at(from);
	bool ok = m_output_values.insert(OutputValueMap::value_type(to, value)).second;
	BLI_assert(ok && "Value for node output already defined!");
	UNUSED_VARS(ok);
}

void LLVMTextureCompiler::append_output_arguments(std::vector<llvm::Value*> &args, const ConstOutputKey &output)
{
	DualValue val = m_output_values.at(output);
	args.push_back(val.value());
	args.push_back(val.dx());
	args.push_back(val.dy());
}

void LLVMTextureCompiler::append_input_value(llvm::BasicBlock *block, std::vector<llvm::Value*> &args,
                                             const TypeSpec *typespec, const ConstOutputKey &link)
{
	using namespace llvm;
	
	IRBuilder<> builder(context());
	builder.SetInsertPoint(block);
	
	DualValue ptr = m_output_values.at(link);
	if (use_argument_pointer(typespec, false)) {
		args.push_back(ptr.value());
		args.push_back(ptr.dx());
		args.push_back(ptr.dy());
	}
	else {
		args.push_back(builder.CreateLoad(ptr.value()));
		args.push_back(builder.CreateLoad(ptr.dx()));
		args.push_back(builder.CreateLoad(ptr.dy()));
	}
}

void LLVMTextureCompiler::append_input_constant(llvm::BasicBlock *block, std::vector<llvm::Value*> &args,
                                                const TypeSpec *typespec, const NodeConstant *node_value)
{
	using namespace llvm;
	
	IRBuilder<> builder(context());
	builder.SetInsertPoint(block);
	
	/* create storage for the global value */
	Constant *cvalue = bvm_create_llvm_constant(context(), node_value);
	
	if (use_argument_pointer(typespec, false)) {
		AllocaInst *pvalue = builder.CreateAlloca(cvalue->getType());
		builder.CreateStore(cvalue, pvalue);
		
		args.push_back(pvalue);
	}
	else {
		args.push_back(cvalue);
	}
}

void LLVMTextureCompiler::map_argument(llvm::BasicBlock *block, const OutputKey &output, llvm::Argument *arg)
{
	using namespace llvm;
	
	const TypeSpec *typespec = output.socket->typedesc.get_typespec();
	
	IRBuilder<> builder(context());
	builder.SetInsertPoint(block);
	
	if (bvm_type_has_dual_value(typespec)) {
		/* argument is a struct, use GEP instructions to get the individual elements */
		m_output_values[output] = DualValue(builder.CreateStructGEP(arg, 0),
		                                    builder.CreateStructGEP(arg, 1),
		                                    builder.CreateStructGEP(arg, 2));
	}
	else {
		m_output_values[output] = DualValue(arg, NULL, NULL);
	}
}

void LLVMTextureCompiler::store_return_value(llvm::BasicBlock *block, const OutputKey &output, llvm::Value *arg)
{
	using namespace llvm;
	
	IRBuilder<> builder(context());
	builder.SetInsertPoint(block);
	
	Value *value_ptr = builder.CreateStructGEP(arg, 0);
	Value *dx_ptr = builder.CreateStructGEP(arg, 1);
	Value *dy_ptr = builder.CreateStructGEP(arg, 2);
	
	DualValue dual = m_output_values.at(output);
	Value *rvalue = builder.CreateLoad(dual.value());
	Value *rdx = builder.CreateLoad(dual.dx());
	Value *rdy = builder.CreateLoad(dual.dy());
	builder.CreateStore(rvalue, value_ptr);
	builder.CreateStore(rdx, dx_ptr);
	builder.CreateStore(rdy, dy_ptr);
}

llvm::Type *LLVMTextureCompiler::get_argument_type(const TypeSpec *spec) const
{
	llvm::Type *type = bvm_get_llvm_type(context(), spec, true);
	if (use_argument_pointer(spec, true))
		type = type->getPointerTo();
	return type;
}

llvm::Type *LLVMTextureCompiler::get_return_type(const TypeSpec *spec) const
{
	return bvm_get_llvm_type(context(), spec, true);
}

void LLVMTextureCompiler::append_input_types(std::vector<llvm::Type*> &params,
                                             const TypeSpec *spec, bool is_constant) const
{
	llvm::Type *type = bvm_get_llvm_type(context(), spec, false);
	if (use_argument_pointer(spec, false))
		type = type->getPointerTo();
	
	params.push_back(type);
	if (!is_constant && bvm_type_has_dual_value(spec)) {
		/* two derivatives */
		params.push_back(type);
		params.push_back(type);
	}
}

void LLVMTextureCompiler::append_output_types(std::vector<llvm::Type*> &params, const TypeSpec *spec) const
{
	using namespace llvm;
	
	Type *type = bvm_get_llvm_type(context(), spec, false);
	params.push_back(type);
	if (bvm_type_has_dual_value(spec)) {
		/* two derivatives */
		params.push_back(type);
		params.push_back(type);
	}
}

bool LLVMTextureCompiler::use_argument_pointer(const TypeSpec *typespec, bool use_dual) const
{
	using namespace llvm;
	
	if (use_dual && bvm_type_has_dual_value(typespec)) {
		/* pass by reference */
		return true;
	}
	else if (typespec->is_aggregate() || typespec->is_structure()) {
		/* pass by reference */
		return true;
	}
	else {
		/* pass by value */
		return false;
	}
}

bool LLVMTextureCompiler::use_elementary_argument_pointer(const TypeSpec *typespec) const
{
	using namespace llvm;
	
	if (typespec->is_aggregate() || typespec->is_structure()) {
		/* pass by reference */
		return true;
	}
	else {
		/* pass by value */
		return false;
	}
}

/* ------------------------------------------------------------------------- */

void LLVMTextureCompiler::define_node_function(llvm::Module *mod, OpCode op, const string &nodetype_name)
{
	const NodeType *nodetype = NodeGraph::find_node_type(nodetype_name);
	if (nodetype == NULL)
		return;
	
	switch (op) {
		/* special cases */
		case OP_GET_DERIVATIVE_FLOAT:
		case OP_GET_DERIVATIVE_FLOAT3:
		case OP_GET_DERIVATIVE_FLOAT4:
			define_get_derivative(mod, op, nodetype);
			break;
		
		default:
			define_elementary_functions(mod, op, nodetype);
			define_dual_function_wrapper(mod, op, nodetype);
			break;
	}
}

void LLVMTextureCompiler::define_nodes_module()
{
	using namespace llvm;
	
	Module *mod = new llvm::Module("texture_nodes", context());
	
#define DEF_OPCODE(op) \
	define_node_function(mod, OP_##op, STRINGIFY(op));
	
	BVM_DEFINE_OPCODES
	
#undef DEF_OPCODE
	
	m_nodes_module = mod;
}

/* ------------------------------------------------------------------------- */

llvm::Function *LLVMTextureCompiler::declare_elementary_node_function(
        llvm::Module *mod, const NodeType *nodetype, const string &name,
        bool with_derivatives)
{
	using namespace llvm;
	
	bool error = false;
	
	std::vector<Type *> input_types, output_types;
	for (int i = 0; i < nodetype->num_inputs(); ++i) {
		const NodeInput *input = nodetype->find_input(i);
		const TypeSpec *typespec = input->typedesc.get_typespec();
		
		Type *type = bvm_get_llvm_type(context(), typespec, false);
		if (type == NULL) {
			error = true;
			break;
		}
		if (use_elementary_argument_pointer(typespec))
			type = type->getPointerTo();
		
		input_types.push_back(type);
		if (with_derivatives &&
		    input->value_type != INPUT_CONSTANT && bvm_type_has_dual_value(typespec)) {
			/* second argument for derivative */
			input_types.push_back(type);
		}
	}
	for (int i = 0; i < nodetype->num_outputs(); ++i) {
		const NodeOutput *output = nodetype->find_output(i);
		const TypeSpec *typespec = output->typedesc.get_typespec();
		
		Type *type = bvm_get_llvm_type(context(), typespec, false);
		if (type == NULL) {
			error = true;
			break;
		}
		
		if (with_derivatives &&
		    bvm_type_has_dual_value(typespec)) {
			output_types.push_back(type);
		}
		else {
			output_types.push_back(type);
		}
	}
	if (error) {
		/* some arguments could not be handled */
		return NULL;
	}
	
	FunctionType *functype = get_node_function_type(input_types, output_types);
	
	Function *func = Function::Create(functype, Function::ExternalLinkage, name, mod);
	return func;
}

bool LLVMTextureCompiler::set_node_function_impl(OpCode op, const NodeType *UNUSED(nodetype),
                                                 llvm::Function *value_func, llvm::Function *deriv_func)
{
	using namespace llvm;
	
	typedef std::vector<Value*> ValueList;
	
	/* XXX TODO needs implementation for derivatives */
	UNUSED_VARS(deriv_func);
	return false;
	
	ValueList value_args;
	value_args.reserve(value_func->arg_size());
	for (Function::arg_iterator a = value_func->arg_begin(); a != value_func->arg_end(); ++a)
		value_args.push_back(a);
	
	switch (op) {
		case OP_VALUE_FLOAT: {
			BasicBlock *block = BasicBlock::Create(context(), "entry", value_func);
			def_node_VALUE_FLOAT(context(), block, value_args[0], value_args[1]);
			return true;
		}
		case OP_VALUE_INT: {
			BasicBlock *block = BasicBlock::Create(context(), "entry", value_func);
			def_node_VALUE_INT(context(), block, value_args[0], value_args[1]);
			return true;
		}
		case OP_VALUE_FLOAT3: {
			BasicBlock *block = BasicBlock::Create(context(), "entry", value_func);
			def_node_VALUE_FLOAT3(context(), block, value_args[0], value_args[1]);
			return true;
		}
		case OP_VALUE_FLOAT4: {
			BasicBlock *block = BasicBlock::Create(context(), "entry", value_func);
			def_node_VALUE_FLOAT4(context(), block, value_args[0], value_args[1]);
			return true;
		}
		case OP_VALUE_MATRIX44: {
			BasicBlock *block = BasicBlock::Create(context(), "entry", value_func);
			def_node_VALUE_MATRIX44(context(), block, value_args[0], value_args[1]);
			return true;
		}
		
		default:
			return false;
	}
}

void LLVMTextureCompiler::define_elementary_functions(llvm::Module *mod, OpCode op, const NodeType *nodetype)
{
	using namespace llvm;
	
	/* declare functions */
	Function *value_func = NULL, *deriv_func = NULL;
	
	if (llvm_has_external_impl_value(op)) {
		value_func = declare_elementary_node_function(
		                 mod, nodetype, bvm_value_function_name(nodetype->name()), false);
	}
	
	if (llvm_has_external_impl_deriv(op)) {
		deriv_func = declare_elementary_node_function(
		                 mod, nodetype, bvm_deriv_function_name(nodetype->name()), true);
	}
	
	set_node_function_impl(op, nodetype, value_func, deriv_func);
}

void LLVMTextureCompiler::define_dual_function_wrapper(llvm::Module *mod, OpCode UNUSED(op), const NodeType *nodetype)
{
	using namespace llvm;
	
	/* get evaluation function(s) */
	string value_name = bvm_value_function_name(nodetype->name());
	Function *value_func = llvm_find_external_function(mod, value_name);
	BLI_assert(value_func != NULL && "Could not find node function!");
	
	string deriv_name = bvm_deriv_function_name(nodetype->name());
	Function *deriv_func = llvm_find_external_function(mod, deriv_name);
	
	/* wrapper function */
	Function *func = declare_node_function(mod, nodetype);
	if (func == NULL)
		return;
	
	BasicBlock *block = BasicBlock::Create(context(), "entry", func);
	IRBuilder<> builder(context());
	builder.SetInsertPoint(block);
	
	/* collect arguments for calling internal elementary functions */
	/* arguments for calculating main value and partial derivatives */
	std::vector<Value*> call_args_value, call_args_dx, call_args_dy;
	
	Function::arg_iterator arg_it = func->arg_begin();
	/* output arguments */
	for (int i = 0; i < nodetype->num_outputs(); ++i) {
		const NodeOutput *output = nodetype->find_output(i);
		
		Value *val, *dx, *dy;
		if (bvm_type_has_dual_value(output->typedesc.get_typespec())) {
			val = arg_it++;
			dx = arg_it++;
			dy = arg_it++;
		}
		else {
			val = arg_it++;
			dx = NULL;
			dy = NULL;
		}
		
		call_args_value.push_back(val);
		if (dx != NULL)
			call_args_dx.push_back(dx);
		if (dy != NULL)
			call_args_dy.push_back(dy);
	}
	/* input arguments */
	for (int i = 0; i < nodetype->num_inputs(); ++i) {
		const NodeInput *input = nodetype->find_input(i);
		const TypeSpec *typespec = input->typedesc.get_typespec();
		
		Value *val, *dx, *dy;
		if (input->value_type != INPUT_CONSTANT && bvm_type_has_dual_value(typespec)) {
			val = arg_it++;
			dx = arg_it++;
			dy = arg_it++;
		}
		else {
			val = arg_it++;
			dx = NULL;
			dy = NULL;
		}
		
		call_args_value.push_back(val);
		
		/* derivative functions take input value as well as its derivative */
		call_args_dx.push_back(val);
		if (dx != NULL)
			call_args_dx.push_back(dx);
		
		call_args_dy.push_back(val);
		if (dy != NULL)
			call_args_dy.push_back(dy);
	}
	
	BLI_assert(arg_it == func->arg_end() && "Did not use all the function arguments!");
	
	/* calculate value */
	builder.CreateCall(value_func, call_args_value);
	
	if (deriv_func) {
		builder.CreateCall(deriv_func, call_args_dx);
		builder.CreateCall(deriv_func, call_args_dy);
	}
	else {
		/* zero the derivatives */
		for (int i = 0; i < nodetype->num_outputs(); ++i, ++arg_it) {
			const NodeOutput *output = nodetype->find_output(i);
			const TypeSpec *typespec = output->typedesc.get_typespec();
			
			if (bvm_type_has_dual_value(typespec)) {
				Constant *c = bvm_make_zero(context(), typespec);
				builder.CreateStore(c, call_args_dx[i]);
				builder.CreateStore(c, call_args_dy[i]);
			}
		}
	}
	
	builder.CreateRetVoid();
}

void LLVMTextureCompiler::define_get_derivative(llvm::Module *mod, OpCode UNUSED(op), const NodeType *nodetype)
{
	using namespace llvm;
	
	Function *func = declare_node_function(mod, nodetype);
	if (func == NULL)
		return;
	
	const TypeSpec *typespec = nodetype->find_input(1)->typedesc.get_typespec();
	
	ConstantInt* idx0 = ConstantInt::get(context(), APInt(32, 0));
	ConstantInt* idx1 = ConstantInt::get(context(), APInt(32, 1));
	
	BasicBlock *block = BasicBlock::Create(context(), "entry", func);
	BasicBlock *block_var0 = BasicBlock::Create(context(), "var0", func);
	BasicBlock *block_var1 = BasicBlock::Create(context(), "var1", func);
	BasicBlock *block_end = BasicBlock::Create(context(), "end", func);
	
	Function::arg_iterator arg_it = func->arg_begin();
	Argument *out_val = arg_it++;
	Argument *out_dx = arg_it++;
	Argument *out_dy = arg_it++;
	Argument *var = arg_it++;
	Argument *in_val = arg_it++;
	Argument *in_dx = arg_it++;
	Argument *in_dy = arg_it++;
	UNUSED_VARS(in_val);
	
	{
		IRBuilder<> builder(context());
		builder.SetInsertPoint(block);
		
		/* zero derivatives */
		llvm::Constant *zero = bvm_make_zero(context(), typespec);
		builder.CreateStore(zero, out_dx);
		builder.CreateStore(zero, out_dy);
		
		SwitchInst *sw = builder.CreateSwitch(var, block_end, 2);
		sw->addCase(idx0, block_var0);
		sw->addCase(idx1, block_var1);
	}
	
	{
		IRBuilder<> builder(context());
		builder.SetInsertPoint(block_var0);
		
		Value *data = in_dx;
		if (use_argument_pointer(typespec, false))
			data = builder.CreateLoad(data);
		builder.CreateStore(data, out_val);
		builder.CreateBr(block_end);
	}
	
	{
		IRBuilder<> builder(context());
		builder.SetInsertPoint(block_var1);
		
		Value *data = in_dy;
		if (use_argument_pointer(typespec, false))
			data = builder.CreateLoad(data);
		builder.CreateStore(data, out_val);
		builder.CreateBr(block_end);
	}
	
	{
		IRBuilder<> builder(context());
		builder.SetInsertPoint(block_end);
		
		builder.CreateRetVoid();
	}
}

} /* namespace blenvm */