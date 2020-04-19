//! \file
/*
**  Copyright (c) 2016 - Ponce
**  Authors:
**         Alberto Garcia Illera        agarciaillera@gmail.com
**         Francisco Oca                francisco.oca.gonzalez@gmail.com
**
**  This program is under the terms of the BSD License.
*/

//C++
#include <stdio.h>
#include <string>
#include <iostream>
#include <fstream>
//Used in GetTimeMs64
#ifdef _WIN32
	#include <Windows.h>
#else
	#include <sys/time.h>
	#include <ctime>
#endif


//Triton
#include <triton/api.hpp>
#include <triton/x86Specifications.hpp>

//IDA
#include <idp.hpp>
#include <loader.hpp>
#include <dbg.hpp>
#include <name.hpp>
#include <bytes.hpp>

//Ponce
#include "utils.hpp"
#include "globals.hpp"
#include "context.hpp"
#include "blacklist.hpp"



/*This function is call the first time we are tainting something to enable the trigger, the flags and the tracing*/
void start_tainting_or_symbolic_analysis()
{
	if (!ponce_runtime_status.is_something_tainted_or_symbolize)
	{
		ponce_runtime_status.runtimeTrigger.enable();
		ponce_runtime_status.analyzed_thread = get_current_thread();
		ponce_runtime_status.is_something_tainted_or_symbolize = true;
		enable_step_trace(true);
		set_step_trace_options(0);
		ponce_runtime_status.tracing_start_time = 0;
	}
}

/*This functions gets a string and return the triton register assign or nullptr
This is using the triton current architecture so it is more generic.*/
const triton::arch::Register* str_to_register(qstring register_name)
{
	auto regs = api.getAllRegisters();
	//for (auto it = regs.begin(); it != regs.end(); it++)
	for (auto& pair : regs)
	{
		auto reg = pair.second;
		//const triton::arch::Register a = it->second;
		//auto b = &a;
		//triton::arch::Register r = it->second;
		if (strcmp(reg.getName().c_str(), register_name.c_str()) == 0)
		{
			return &regs[pair.first];
		}
	}
	return nullptr;
}


/*This function ask to the user to take a snapshot.
It returns:
1: yes
0: No
-1: Cancel execution of script*/
int ask_for_a_snapshot()
{
	//We don't want to ask the user all the times if he wants to take a snapshot or not...
	if (already_exits_a_snapshot())
		return 1;
	while (true)
	{
		int answer = ask_yn(1, "[?] Do you want to take a database snapshot before using the script? (It will color some intructions) (Y/n):");
		if (answer == 1) //Yes
		{
			snapshot_t snapshot;
			qstrncpy(snapshot.desc, SNAPSHOT_DESCRIPTION, MAX_DATABASE_DESCRIPTION);
			qstring errmsg;
			bool success = take_database_snapshot(&snapshot, &errmsg);
			return 1;
		}
		else if (answer == 0) //No
			return 0;
		else //Cancel
			return -1;
	}
}

/*This functions is a helper for already_exits_a_snapshot. This is call for every snapshot found. 
The user data, ud, containt a pointer to a boolean to enable if we find the snapshot.*/
int __stdcall snapshot_visitor(snapshot_t *ss, void *ud)
{
	if (strcmp(ss->desc, SNAPSHOT_DESCRIPTION) == 0)
	{
		bool *exists = (bool *)ud;
		*exists = true;
		return 1;
	}
	return 0;
}

/*This functions check if it exists already a snapshot made by the plugin.
So we don't ask to the user every time he runs the plugin.*/
bool already_exits_a_snapshot()
{
	snapshot_t root;
	bool result = build_snapshot_tree(&root);
	if (!result)
		return false;
	bool exists = false;
	visit_snapshot_tree(&root, &snapshot_visitor, &exists);
	return exists;
}

/*This function is a helper to find a function having its name.
It is likely IDA SDK has another API to do this but I can't find it.
Source: http://www.openrce.org/reference_library/files/ida/idapw.pdf */
ea_t find_function(char const *function_name)
{
	// get_func_qty() returns the number of functions in file(s) loaded.
	for (unsigned int f = 0; f < get_func_qty(); f++) {
		// getn_func() returns a func_t struct for the function number supplied
		func_t *curFunc = getn_func(f);
		qstring funcName;
		ssize_t size_read = 0;
		// get_func_name2 gets the name of a function and stored it in funcName
		size_read = get_func_name(&funcName, curFunc->start_ea);
		if (size_read > 0) { // if found
			if (strcmp(funcName.c_str(), function_name) == 0) {
				return curFunc->start_ea;
			}
			//We need to ignore our prefix when the function is tainted
			//If the function name starts with our prefix, fix for #51
			if (strstr(funcName.c_str(), RENAME_TAINTED_FUNCTIONS_PREFIX) == funcName.c_str() && funcName.size() > RENAME_TAINTED_FUNCTIONS_PATTERN_LEN) {
				//Then we ignore the prefix and compare the rest of the function name
				if (strcmp(funcName.c_str() + RENAME_TAINTED_FUNCTIONS_PATTERN_LEN, function_name) == 0) {
					return curFunc->start_ea;
				}
			}
		}
	}
	return -1;
}

//This function return the real value of the argument.
ea_t get_args(int argument_number, bool skip_ret)
{
#if !defined(__EA64__)
	ea_t memprogram = get_args_pointer(argument_number, skip_ret);
	//We first get the pointer and then we dereference it
	ea_t value = 0;
	value = read_regSize_from_ida(memprogram);
	return value;

#else
	int skip_ret_index = skip_ret ? 1 : 0;
	//Not converted to IDA we should use get_reg_val
#ifdef __NT__ // note the underscore: without it, it's not msdn official!
	// On Windows - function parameters are passed in using RCX, RDX, R8, R9 for ints / ptrs and xmm0 - 3 for float types.
	switch (argument_number)
	{
	case 0: return IDA_getCurrentRegisterValue(api.registers.x86_rcx).convert_to<ea_t>();
	case 1: return IDA_getCurrentRegisterValue(api.registers.x86_rdx).convert_to<ea_t>();
	case 2: return IDA_getCurrentRegisterValue(api.registers.x86_r8).convert_to<ea_t>();
	case 3: return IDA_getCurrentRegisterValue(api.registers.x86_r9).convert_to<ea_t>();
	default:
		ea_t esp = (ea_t)IDA_getCurrentRegisterValue(api.registers.x86_rsp).convert_to<ea_t>();
		ea_t arg = esp + (argument_number - 4 + skip_ret_index) * 8;
		return get_qword(arg);
	}
#elif __LINUX__ || __MAC__ // IDA macros https://www.hex-rays.com/products/ida/support/sdkdoc/pro_8h.html
	//On Linux - parameters are passed in RDI, RSI, RDX, RCX, R8, R9 for ints / ptrs and xmm0 - 7 for float types.
	switch (argument_number)
	{
	case 0: return IDA_getCurrentRegisterValue(api.registers.x86_rdi).convert_to<ea_t>();
	case 1: return IDA_getCurrentRegisterValue(api.registers.x86_rsi).convert_to<ea_t>();
	case 2: return IDA_getCurrentRegisterValue(api.registers.x86_rdx).convert_to<ea_t>();
	case 3: return IDA_getCurrentRegisterValue(api.registers.x86_rcx).convert_to<ea_t>();
	case 4: return IDA_getCurrentRegisterValue(api.registers.x86_r8).convert_to<ea_t>();
	case 5: return IDA_getCurrentRegisterValue(api.registers.x86_r9).convert_to<ea_t>();
	default:
		ea_t esp = (ea_t)IDA_getCurrentRegisterValue(api.registers.x86_rsp);
		ea_t arg = esp + (argument_number - 6 + skip_ret_index) * 8;
		return get_qword(arg);
	}
#endif
#endif
}

// Return the argument at the "argument_number" position. It is independant of the architecture and the OS.
// We suppossed the function is using the default call convention, stdcall or cdelc in x86, no fastcall and fastcall in x64
ea_t get_args_pointer(int argument_number, bool skip_ret)
{
	int skip_ret_index = skip_ret ? 1 : 0;
#if !defined(__EA64__)
	regval_t esp_value;
	invalidate_dbg_state(DBGINV_REGS);
	get_reg_val("esp", &esp_value);
	ea_t arg = (ea_t)esp_value.ival + (argument_number + skip_ret_index) * 4;
	return arg;
#else
	//Not converted to IDA we should use get_reg_val
#ifdef __NT__ // note the underscore: without it, it's not msdn official!
	// On Windows - function parameters are passed in using RCX, RDX, R8, R9 for ints / ptrs and xmm0 - 3 for float types.
	switch (argument_number)
	{
	case 0: 
	case 1: 
	case 2: 
	case 3: error("[!] In Windows 64 bits you can't get a pointer to the four first\n arguments since they are registers");
	default:
		ea_t esp = (ea_t)IDA_getCurrentRegisterValue(api.registers.x86_rsp).convert_to<ea_t>();
		ea_t arg = esp + (argument_number - 4 + skip_ret_index) * 8;
		return arg;
	}
#elif __LINUX__ || __MAC__ // IDA macros https://www.hex-rays.com/products/ida/support/sdkdoc/pro_8h.html
	//On Linux - parameters are passed in RDI, RSI, RDX, RCX, R8, R9 for ints / ptrs and xmm0 - 7 for float types.
	switch (argument_number)
	{
	case 0: 
	case 1: 
	case 2: 
	case 3: 
	case 4: 
	case 5:error("[!] In Linux/OsX 64 bits you can't get a pointer to the five first\n arguments since they are registers");
	default:
		ea_t esp = (ea_t)IDA_getCurrentRegisterValue(api.registers.x86_rsp);
		ea_t arg = esp + (argument_number - 6 + skip_ret_index) * 8;
		return arg;
	}
#endif
#endif
}

//Use templates??
short read_unicode_char_from_ida(ea_t address)
{
	short value;
	//This is the way to force IDA to read the value from the debugger
	//More info here: https://www.hex-rays.com/products/ida/support/sdkdoc/dbg_8hpp.html#ac67a564945a2c1721691aa2f657a908c
	invalidate_dbgmem_contents(address, sizeof(value));
	ssize_t bytes_read = get_bytes(&value, sizeof(value), address, GMB_READALL, NULL);
	if (bytes_read == 0 || bytes_read == -1) {
		if (inf_is_64bit())
			msg("[!] Error reading memory from %#llx\n", address);
		else
			msg("[!] Error reading memory from %#x\n", address);
	}
	return value;
}

//Use templates??
char read_char_from_ida(ea_t address)
{
	char value;
	//This is the way to force IDA to read the value from the debugger
	//More info here: https://www.hex-rays.com/products/ida/support/sdkdoc/dbg_8hpp.html#ac67a564945a2c1721691aa2f657a908c
	invalidate_dbgmem_contents(address, sizeof(value));
	ssize_t bytes_read = get_bytes(&value, sizeof(value), address, GMB_READALL, NULL);
	if (bytes_read == 0 || bytes_read == -1) {
		if (inf_is_64bit())
			msg("[!] Error reading memory from %#llx\n", address);
		else
			msg("[!] Error reading memory from %#x\n", address);
	}
	return value;
}

ea_t read_regSize_from_ida(ea_t address)
{
	ea_t value;
	//This is the way to force IDA to read the value from the debugger
	//More info here: https://www.hex-rays.com/products/ida/support/sdkdoc/dbg_8hpp.html#ac67a564945a2c1721691aa2f657a908c
	invalidate_dbgmem_contents(address, sizeof(value));
	ssize_t bytes_read = get_bytes(&value, sizeof(value), address, GMB_READALL, NULL);
	if (bytes_read == 0 || bytes_read == -1) {
		if (inf_is_64bit())
			msg("[!] Error reading memory from %#llx\n", address);
		else
			msg("[!] Error reading memory from %#x\n", address);
	}

	return value;
}

/*This function renames a tainted function with the prefix RENAME_TAINTED_FUNCTIONS_PATTERN, by default "T%03d_"*/
void rename_tainted_function(ea_t address)
{
	qstring func_name;
	ssize_t size = 0x0;
	//First we get the current function name
	size = get_func_name(&func_name, address);

	if (size > 0)
	{
		//If the function isn't already renamed
		if (strstr(func_name.c_str(), RENAME_TAINTED_FUNCTIONS_PREFIX) != func_name.c_str())
		{
			char new_func_name[MAXSTR];
			//This is a bit tricky, the prefix contains the format string, so if the user modified it and removes the format string isn't going to work
			qsnprintf(new_func_name, sizeof(new_func_name), RENAME_TAINTED_FUNCTIONS_PATTERN"%s", ponce_runtime_status.tainted_functions_index, func_name.c_str());
			//We need the start of the function we can have that info with our function find_function
			set_name(find_function(func_name.c_str()), new_func_name);
			if (cmdOptions.showDebugInfo)
				msg("[+] Renaming function %s -> %s\n", func_name.c_str(), new_func_name);
			ponce_runtime_status.tainted_functions_index += 1;
		}
	}
}

void add_symbolic_expressions(triton::arch::Instruction* tritonInst, ea_t address)
{
	for (unsigned int exp_index = 0; exp_index != tritonInst->symbolicExpressions.size(); exp_index++) 
	{
		auto expr = tritonInst->symbolicExpressions[exp_index];
		std::ostringstream oss;
		oss << expr;
		add_extra_cmt(address, false, "%s", oss.str().c_str());
	}
}

std::string notification_code_to_string(int notification_code)
{
	switch (notification_code)
	{
		case 0:
			return std::string("dbg_null");
		case 1:
			return std::string("dbg_process_start");
		case 2:
			return std::string("dbg_process_exit");
		case 3:
			return std::string("dbg_process_attach");
		case 4:
			return std::string("dbg_process_detach");
		case 5:
			return std::string("dbg_thread_start");
		case 6:
			return std::string("dbg_thread_exit");
		case 7:
			return std::string("dbg_library_load");
		case 8:
			return std::string("dbg_library_unload");
		case 9:
			return std::string("dbg_information");
		case 10:
			return std::string("dbg_exception");
		case 11:
			return std::string("dbg_suspend_process");
		case 12:
			return std::string("dbg_bpt");
		case 13:
			return std::string("dbg_trace");
		case 14:
			return std::string("dbg_request_error");
		case 15:
			return std::string("dbg_step_into");
		case 16:
			return std::string("dbg_step_over");
		case 17:
			return std::string("dbg_run_to");
		case 18:
			return std::string("dbg_step_until_ret");
		case 19:
			return std::string("dbg_bpt_changed");
		case 20:
			return std::string("dbg_last");
		default:
			return std::string("Not defined");
	}
}

/*This function loads the options from the config file.
It returns true if it reads the config false, if there is any error.*/
bool load_options(struct cmdOptionStruct *cmdOptions)
{
	std::ifstream config_file;
	config_file.open("Ponce.cfg", std::ios::in | std::ios::binary);
	if (!config_file.is_open())
	{
		msg("[!] Config file %s not found\n", "Ponce.cfg");
		return false;
	}
	auto begin = config_file.tellg();
	config_file.seekg(0, std::ios::end);
	auto end = config_file.tellg();
	config_file.seekg(0, std::ios::beg);
	if ((end - begin) != sizeof(struct cmdOptionStruct))
		return false;
	config_file.read((char *)cmdOptions, sizeof(struct cmdOptionStruct));
	config_file.close();

	//Check if we need to reload the custom blacklisted functions
	if (cmdOptions->blacklist_path[0] != '\0'){
		//Means that the user set a path for custom blacklisted functions
		if (blacklkistedUserFunctions != NULL){
			//Check if we had a previous custom blacklist and if so we delete it
			blacklkistedUserFunctions->clear();
			delete blacklkistedUserFunctions;
			blacklkistedUserFunctions = NULL;
		}
		readBlacklistfile(cmdOptions->blacklist_path);
	}

	return true;
}

/*This function loads the options from the config file.
It returns true if it reads the config false, if there is any error.*/
bool save_options(struct cmdOptionStruct *cmdOptions)
{
	std::ofstream config_file;
	config_file.open("Ponce.cfg", std::ios::out | std::ios::binary);
	if (!config_file.is_open())
	{
		msg("[!] Error opening config file %s\n", "Ponce.cfg");
		return false;
	}
	config_file.write((char *)cmdOptions, sizeof(struct cmdOptionStruct));
	config_file.close();
	return true;
}


/*Solve a formula and returns the solution as an input.
The bound is an index in the myPathConstrains vector*/
Input * solve_formula(ea_t pc, uint bound)
{
	auto ast = api.getAstContext();

	auto path_constraint = ponce_runtime_status.myPathConstraints[bound];
	if (path_constraint.conditionAddr == pc)
	{
		std::vector <triton::ast::SharedAbstractNode> expr;
		//First we add to the expresion all the previous path constrains
		unsigned int j;
		for (j = 0; j < bound; j++)
		{
			if (cmdOptions.showExtraDebugInfo)
				msg("[+] Keeping condition %d\n", j);
			triton::usize ripId = ponce_runtime_status.myPathConstraints[j].conditionRipId;
			auto symExpr = api.getSymbolicExpression(ripId)->getAst();
			ea_t takenAddr = ponce_runtime_status.myPathConstraints[j].takenAddr;
			
			//expr.push_back(triton::ast::assert_(triton::ast::equal(symExpr, triton::ast::bv(takenAddr, symExpr->getBitvectorSize()))));
			expr.push_back(ast->equal(symExpr, ast->bv(takenAddr, symExpr->getBitvectorSize())));
		}
		if (cmdOptions.showExtraDebugInfo)
			msg("[+] Inverting condition %d\n", bound);
		//And now we negate the selected condition
		triton::usize ripId = ponce_runtime_status.myPathConstraints[bound].conditionRipId;
		auto symExpr = api.getSymbolicExpression(ripId)->getAst();
		ea_t notTakenAddr = ponce_runtime_status.myPathConstraints[bound].notTakenAddr;
		if (cmdOptions.showExtraDebugInfo) {
			if (inf_is_64bit())
				msg("[+] ripId: %d notTakenAddr: %#llx\n", ripId, notTakenAddr);
			else
				msg("[+] ripId: %d notTakenAddr: %#x\n", ripId, notTakenAddr);
		}
		expr.push_back(ast->equal(symExpr, ast->bv(notTakenAddr, symExpr->getBitvectorSize())));

		//Time to solve
		auto final_expr = ast->compound(expr);

		if (cmdOptions.showDebugInfo)
			msg("[+] Solving formula...\n");

		if (cmdOptions.showExtraDebugInfo)
		{
			std::stringstream ss;
			/*Create the full formula*/
			ss << "(set-logic QF_AUFBV)\n";
			/* Then, delcare all symbolic variables */
			for (auto it : api.getSymbolicVariables()) {
				ss << ast->declare(ast->variable(it.second));
				
			}
			/* And concat the user expression */
			ss << "\n\n";
			ss << final_expr;
			ss << "\n(check-sat)";
			ss << "\n(get-model)";
			msg("[+] Formula: %s\n", ss.str().c_str());
		}

		auto model = api.getModel(final_expr);

		if (model.size() > 0)
		{
			Input *newinput = new Input();
			//Clone object 
			newinput->bound = path_constraint.bound;

			msg("[+] Solution found! Values:\n");
			for (auto it = model.begin(); it != model.end(); it++)
			{
				// ToDo: check this for loop bc I feel the conversion did not go well
				auto symId = it->first;
				auto model = it->second;

				triton::engines::symbolic::SharedSymbolicVariable  symbVar = api.getSymbolicVariable(symId);
				std::string  symbVarComment = symbVar->getComment();
				triton::engines::symbolic::variable_e symbVarKind = symbVar->getType();
				triton::uint512 model_value = model.getValue();
				if (symbVarKind == triton::engines::symbolic::variable_e::MEMORY_VARIABLE)
				{
					auto mem = triton::arch::MemoryAccess(symbVar->getOrigin(), symbVar->getSize() / 8);
					newinput->memOperand.push_back(mem);
					api.setConcreteMemoryValue(mem, model_value);
				}
				else if (symbVarKind == triton::engines::symbolic::variable_e::REGISTER_VARIABLE){
					auto reg = triton::arch::Register(*api.getCpuInstance(), (triton::arch::register_e)symbVar->getOrigin());
					newinput->regOperand.push_back(reg);
					api.setConcreteRegisterValue(reg, model_value);
					//ToDo: add concretizeRegister()??
				}
				//We represent the number different 
				switch (symbVar->getSize())
				{
				case 8:
					msg(" - %s (%s):%#02x (%c)\n", it->second.getVariable()->getName().c_str(), symbVarComment.c_str(), model_value.convert_to<uchar>(), model_value.convert_to<uchar>() == 0? ' ': model_value.convert_to<uchar>());
					break;
				case 16:
					msg(" - %s (%s):%#04x (%c%c)\n", it->second.getVariable()->getName().c_str(), symbVarComment.c_str(), model_value.convert_to<ushort>(), model_value.convert_to<uchar>() == 0 ? ' ' : model_value.convert_to<uchar>(), (unsigned char)(model_value.convert_to<ushort>() >> 8) == 0 ? ' ': (unsigned char)(model_value.convert_to<ushort>() >> 8));
					break;
				case 32:
					msg(" - %s (%s):%#08x\n", it->second.getVariable()->getName().c_str(), symbVarComment.c_str(), model_value.convert_to<uint32>());
					break;
				case 64:
					msg(" - %s (%s):%#16llx\n", it->second.getVariable()->getName().c_str(), symbVarComment.c_str(), model_value.convert_to<uint64>());
					break;
				default:
					msg("[!] Unsupported size for the symbolic variable: %s (%s)\n", it->second.getVariable()->getName().c_str(), symbVarComment.c_str());
				}
			}
			return newinput;
		}
		else
			msg("[!] No solution found :(\n");
	}
	return NULL;
}



/*This function identify the type of condition jmp and negate the flags to negate the jmp.
Probably it is possible to do this with the solver, adding more variable to the formula to 
identify the flag of the conditions and get the values. But for now we are doing it in this way.*/
void negate_flag_condition(triton::arch::Instruction *triton_instruction)
{
	switch (triton_instruction->getType())
	{
	case triton::arch::x86::ID_INS_JA:
	{
		uint64 cf;
		get_reg_val("CF", &cf);
		uint64 zf;
		get_reg_val("ZF", &zf);
		if (cf == 0 && zf == 0)
		{
			cf = 1;
			zf = 1;
		}
		else
		{
			cf = 0;
			zf = 0;
		}
		set_reg_val("ZF", zf);
		set_reg_val("CF", cf);
		break;
	}
	case triton::arch::x86::ID_INS_JAE:
	{
		uint64 cf;
		get_reg_val("CF", &cf);
		uint64 zf;
		get_reg_val("ZF", &zf);
		if (cf == 0 || zf == 0)
		{
			cf = 1;
			zf = 1;
		}
		else
		{
			cf = 0;
			zf = 0;
		}
		set_reg_val("ZF", zf);
		set_reg_val("CF", cf);
		break;
	}
	case triton::arch::x86::ID_INS_JB:
	{
		uint64 cf;
		get_reg_val("CF", &cf);
		cf = !cf;
		set_reg_val("CF", cf);
		break;
	}
	case triton::arch::x86::ID_INS_JBE:
	{
		uint64 cf;
		get_reg_val("CF", &cf);
		uint64 zf;
		get_reg_val("ZF", &zf);
		if (cf == 1 || zf == 1)
		{
			cf = 0;
			zf = 0;
		}
		else
		{
			cf = 1;
			zf = 1;
		}
		set_reg_val("ZF", zf);
		set_reg_val("CF", cf);
		break;
	}
	/*	ToDo: Check this one
		case triton::arch::x86::ID_INS_JCXZ:
		{
		break;
		}*/
	case triton::arch::x86::ID_INS_JE:
	case triton::arch::x86::ID_INS_JNE:
	{
		uint64 zf;
		auto old_value = get_reg_val("ZF", &zf);
		zf = !zf;
		set_reg_val("ZF", zf);
		break;
	}
	//case triton::arch::x86::ID_INS_JRCXZ:
	//case triton::arch::x86::ID_INS_JECXZ:
	case triton::arch::x86::ID_INS_JG:
	{
		uint64 sf;
		get_reg_val("SF", &sf);
		uint64 of;
		get_reg_val("OF", &of);
		uint64 zf;
		get_reg_val("ZF", &zf);
		if (sf == of && zf == 0)
		{
			sf = !of;
			zf = 1;
		}
		else
		{
			sf = of;
			zf = 0;
		}
		set_reg_val("SF", sf);
		set_reg_val("OF", of);
		set_reg_val("ZF", zf);
		break;
	}
	case triton::arch::x86::ID_INS_JGE:
	{
		uint64 sf;
		get_reg_val("SF", &sf);
		uint64 of;
		get_reg_val("OF", &of);
		uint64 zf;
		get_reg_val("ZF", &zf);
		if (sf == of || zf == 1)
		{
			sf = !of;
			zf = 0;
		}
		else
		{
			sf = of;
			zf = 1;
		}
		set_reg_val("SF", sf);
		set_reg_val("OF", of);
		set_reg_val("ZF", zf);
		break;
	}
	case triton::arch::x86::ID_INS_JL:
	{
		uint64 sf;
		get_reg_val("SF", &sf);
		uint64 of;
		get_reg_val("OF", &of);
		if (sf == of)
		{
			sf = !of;
		}
		else
		{
			sf = of;
		}
		set_reg_val("SF", sf);
		set_reg_val("OF", of);
		break;
	}
	case triton::arch::x86::ID_INS_JLE:
	{
		uint64 sf;
		get_reg_val("SF", &sf);
		uint64 of;
		get_reg_val("OF", &of);
		uint64 zf;
		get_reg_val("ZF", &zf);
		if (sf != of || zf == 1)
		{
			sf = of;
			zf = 0;
		}
		else
		{
			sf = !of;
			zf = 1;
		}
		set_reg_val("SF", sf);
		set_reg_val("OF", of);
		set_reg_val("ZF", zf);
		break;
	}
	case triton::arch::x86::ID_INS_JNO:
	case triton::arch::x86::ID_INS_JO:
	{
		uint64 of;
		get_reg_val("OF", &of);
		of = !of;
		set_reg_val("OF", of);
		break;
	}
	case triton::arch::x86::ID_INS_JNP:
	case triton::arch::x86::ID_INS_JP:
	{
		uint64 pf;
		get_reg_val("PF", &pf);
		pf = !pf;
		set_reg_val("PF", pf);
		break;
	}
	case triton::arch::x86::ID_INS_JNS:
	case triton::arch::x86::ID_INS_JS:
	{
		uint64 sf;
		get_reg_val("SF", &sf);
		sf = !sf;
		set_reg_val("SF", sf);
		break;
	}
	}
}

/*Ask to the user if he really want to execute native code even if he has a snapshot.
Returns true if the user say yes.*/
bool ask_for_execute_native()
{
	//Is there any snapshot?
	if (!snapshot.exists())
		return true;
	//If so we should say to the user that he cannot execute native code and expect the snapshot to work
	int answer = ask_yn(1, "[?] If you execute native code (without tracing) Ponce cannot trace all the memory modifications so the execution snapshot will be deleted. Do you still want to do it? (Y/n):");
	if (answer == 1) //Yes
		return true;
	else // No or Cancel
		return false;
}

/*This function deletes the prefixes and sufixes that IDA adds*/
qstring clean_function_name(qstring name){
	if (name.substr(0, 7) == "__imp__")
		return clean_function_name(name.substr(7));
	else if (name.substr(0, 4) == "imp_")
		return clean_function_name(name.substr(4));
	else if (name.substr(0, 3) == "cs:" || name.substr(0, 3) == "ds:")
		return clean_function_name(name.substr(3));
	else if (name.substr(0, 2) == "j_")
		return clean_function_name(name.substr(2));
	else if (name.substr(0, 1) == "_" || name.substr(0, 1) == "@" || name.substr(0, 1) == "?")
		return clean_function_name(name.substr(1));
	else if (name.find('@', 0) != -1)
		return clean_function_name(name.substr(0, name.find('@', 0)));
	else if (name.at(name.length() - 2) == '_' && isdigit(name.at(name.length() - 1))) //name_1
		return clean_function_name(name.substr(0, name.length() - 2));
	return name;
}

qstring get_callee_name(ea_t address) {
	qstring name;
	char buf[100] = {0};
	static const char * nname = "$ vmm functions";
	netnode n(nname);
	auto fun = n.altval(address) - 1;
	if (fun == -1) {
		qstring buf_op;
		if (is_code(get_flags(address)))
			print_operand(&buf_op, address, 0);
		qstring buf_tag;
		tag_remove(&buf_tag, buf_op);
		name = clean_function_name(buf_tag);
	}
	else {
		get_ea_name(&name, fun); // 00C5101A call    edi ; __imp__malloc style
		
		if (name.empty()) {
			qstring buf_op;
			if (is_code(get_flags(address)))
				print_operand(&buf_op, address, 0);
			qstring buf_tag;
			tag_remove(&buf_tag, buf_op);
			name = clean_function_name(buf_tag);
		}
		else {
			name = clean_function_name(name);
		}
	}
	return name;	
}

regval_t ida_get_reg_val_invalidate(char *reg_name)
{
	regval_t reg_value;
	invalidate_dbg_state(DBGINV_REGS);
	get_reg_val(reg_name, &reg_value);
	return reg_value;
}

std::uint64_t GetTimeMs64(void)
{
#ifdef _WIN32
	/* Windows */
	FILETIME ft;
	LARGE_INTEGER li;

	/* Get the amount of 100 nano seconds intervals elapsed since January 1, 1601 (UTC) and copy it
	* to a LARGE_INTEGER structure. */
	GetSystemTimeAsFileTime(&ft);
	li.LowPart = ft.dwLowDateTime;
	li.HighPart = ft.dwHighDateTime;

	std::uint64_t ret = li.QuadPart;
	ret -= 116444736000000000LL; /* Convert from file time to UNIX epoch time. */
	ret /= 10000; /* From 100 nano seconds (10^-7) to 1 millisecond (10^-3) intervals */

	return ret;
#else
	/* Linux */
	struct timeval tv;

	gettimeofday(&tv, NULL);

	triton::uint64 ret = tv.tv_usec;
	/* Convert from micro seconds (10^-6) to milliseconds (10^-3) */
	ret /= 1000;

	/* Adds the seconds (10^0) after converting them to milliseconds (10^-3) */
	ret += (tv.tv_sec * 1000);

	return ret;
#endif
}

/*We need this helper because triton doesn't allow to symbolize memory regions unalinged, so we symbolize every byte*/
void symbolize_all_memory(ea_t address, ea_t size)
{
	// ToDo: add a proper comment on each symbolized memory
	for (unsigned int i = 0; i < size; i++)
	{		
		//api.symbolizeMemory(triton::arch::MemoryAccess(address+i, 1), comment);
		auto symVar = api.symbolizeMemory(triton::arch::MemoryAccess(address + i, 1));
		auto var_name = symVar->getName();
		set_cmt(address + i, var_name.c_str(), true);
	}
}

/* Gets current instruction. Only possible if */
ea_t current_instruction()
{
	if (!is_debugger_on()) {
		return 0;
	}
	ea_t xip = 0;
	if (!get_ip_val(&xip)) {
		msg("Could not get the XIP value\n This should never happen");
		return 0;
	}
	return xip;
}
