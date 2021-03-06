/*
	Aseba - an event-based framework for distributed robot control
	Copyright (C) 2007--2011:
		Stephane Magnenat <stephane at magnenat dot net>
		(http://stephane.magnenat.net)
		and other contributors, see authors.txt for details
	
	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU Lesser General Public License as published
	by the Free Software Foundation, version 3 of the License.
	
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Lesser General Public License for more details.
	
	You should have received a copy of the GNU Lesser General Public License
	along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "compiler.h"
#include "tree.h"
#include "../common/consts.h"
#include "../utils/utils.h"
#include "../utils/FormatableString.h"
#include <cassert>
#include <cstdlib>
#include <sstream>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <memory>
#include <limits>

namespace Aseba
{
	/** \addtogroup compiler */
	/*@{*/
	
	//! Compute the XModem CRC of the description, as defined in AS001 at https://aseba.wikidot.com/asebaspecifications
	uint16 TargetDescription::crc() const
	{
		uint16 crc(0);
		crc = crcXModem(crc, bytecodeSize);
		crc = crcXModem(crc, variablesSize);
		crc = crcXModem(crc, stackSize);
		for (size_t i = 0; i < namedVariables.size(); ++i)
		{
			crc = crcXModem(crc, namedVariables[i].size);
			crc = crcXModem(crc, namedVariables[i].name);
		}
		for (size_t i = 0; i < localEvents.size(); ++i)
		{
			crc = crcXModem(crc, localEvents[i].name);
		}
		for (size_t i = 0; i < nativeFunctions.size(); ++i)
		{
			crc = crcXModem(crc, nativeFunctions[i].name);
			for (size_t j = 0; j < nativeFunctions[i].parameters.size(); ++j)
			{
				crc = crcXModem(crc, nativeFunctions[i].parameters[j].size);
				crc = crcXModem(crc, nativeFunctions[i].parameters[j].name);
			}
		}
		return crc;
	}
	
	//! Constructor. You must setup a description using setTargetDescription() before any call to compile().
	Compiler::Compiler()
	{
		targetDescription = 0;
		commonDefinitions = 0;
	}
	
	//! Set the description of the target as returned by the microcontroller. You must call this function before any call to compile().
	void Compiler::setTargetDescription(const TargetDescription *description)
	{
		targetDescription = description;
	}
	
	//! Set the common definitions, such as events or some constants
	void Compiler::setCommonDefinitions(const CommonDefinitions *definitions)
	{
		commonDefinitions = definitions;
	}
	
	//! Compile a new condition
	//! \param source stream to read the source code from
	//! \param bytecode destination array for bytecode
	//! \param allocatedVariablesCount amount of allocated variables
	//! \param errorDescription error is copied there on error
	//! \param dump stream to send dump messages to
	//! \return returns true on success 
	bool Compiler::compile(std::wistream& source, BytecodeVector& bytecode, unsigned& allocatedVariablesCount, Error &errorDescription, std::wostream* dump)
	{
		assert(targetDescription);
		assert(commonDefinitions);
		
		Node *program;
		unsigned indent = 0;
		
		// we need to build maps at each compilation in case previous ones produced errors and messed maps up
		buildMaps();
		if (freeVariableIndex > targetDescription->variablesSize)
		{
			errorDescription = Error(SourcePos(), L"Broken target description: not enough room for internal variables");
			return false;
		}
		
		// tokenization
		try
		{
			tokenize(source);
		}
		catch (Error error)
		{
			errorDescription = error;
			return false;
		}
		
		if (dump)
		{
			*dump << "Dumping tokens:\n";
			dumpTokens(*dump);
			*dump << "\n\n";
		}
		
		// parsing
		try
		{
			program = parseProgram();
		}
		catch (Error error)
		{
			errorDescription = error;
			return false;
		}
		
		if (dump)
		{
			*dump << "Syntax tree before optimisation:\n";
			program->dump(*dump, indent);
			*dump << "\n\n";
			*dump << "Type checking:\n";
		}
		
		// typecheck
		try
		{
			program->typeCheck();
		}
		catch(Error error)
		{
			delete program;
			errorDescription = error;
			return false;
		}
		
		if (dump)
		{
			*dump << "correct.\n";
			*dump << "\n\n";
			*dump << "Optimizations:\n";
		}
		
		// optimization
		try
		{
			program = program->optimize(dump);
		}
		catch (Error error)
		{
			delete program;
			errorDescription = error;
			return false;
		}
		
		if (dump)
		{
			*dump << "\n\n";
			*dump << "Syntax tree after optimization:\n";
			program->dump(*dump, indent);
			*dump << "\n\n";
		}
		
		// set the number of allocated variables
		allocatedVariablesCount = freeVariableIndex;
		
		// code generation
		PreLinkBytecode preLinkBytecode;
		program->emit(preLinkBytecode);
		delete program;
		
		// fix-up (add of missing STOP and RET bytecodes at code generation)
		preLinkBytecode.fixup(subroutineTable);
		
		// stack check
		if (!verifyStackCalls(preLinkBytecode))
		{
			errorDescription = Error(SourcePos(), L"Execution stack will overflow, check for any recursive subroutine call and cut long mathematical expressions.");
			return false;
		}
		
		// linking (flattening of complex structure into linear vector)
		if (!link(preLinkBytecode, bytecode))
		{
			errorDescription = Error(SourcePos(), L"Script too big for target bytecode size.");
			return false;
		}
		
		if (dump)
		{
			*dump << "Bytecode:\n";
			disassemble(bytecode, preLinkBytecode, *dump);
			*dump << "\n\n";
		}
		
		return true;
	}
	
	//! Create the final bytecode for a microcontroller
	bool Compiler::link(const PreLinkBytecode& preLinkBytecode, BytecodeVector& bytecode)
	{
		bytecode.clear();
		
		// event vector table size
		unsigned addr = preLinkBytecode.events.size() * 2 + 1;
		bytecode.push_back(addr);
		
		// events
		for (PreLinkBytecode::EventsBytecode::const_iterator it = preLinkBytecode.events.begin();
			it != preLinkBytecode.events.end();
			++it
		)
		{
			bytecode.push_back(it->first);		// id
			bytecode.push_back(addr);			// addr
			addr += it->second.size();			// next bytecode addr
		}
		
		// evPreLinkBytecode::ents bytecode
		for (PreLinkBytecode::EventsBytecode::const_iterator it = preLinkBytecode.events.begin();
			it != preLinkBytecode.events.end();
			++it
		)
		{
			std::copy(it->second.begin(), it->second.end(), std::back_inserter(bytecode));
		}
		
		// subrountines bytecode
		for (PreLinkBytecode::SubroutinesBytecode::const_iterator it = preLinkBytecode.subroutines.begin();
			it != preLinkBytecode.subroutines.end();
			++it
		)
		{
			subroutineTable[it->first].address = bytecode.size();
			std::copy(it->second.begin(), it->second.end(), std::back_inserter(bytecode));
		}
		
		// resolve subroutines call addresses
		for (size_t pc = 0; pc < bytecode.size();)
		{
			switch (bytecode[pc] >> 12)
			{
				case ASEBA_BYTECODE_SUB_CALL:
				{
					unsigned id = bytecode[pc] & 0x0fff;
					assert(id < subroutineTable.size());
					unsigned address = subroutineTable[id].address;
					bytecode[pc].bytecode &= 0xf000;
					bytecode[pc].bytecode |= address;
					pc += 1;
				}
				break;
				
				case ASEBA_BYTECODE_LARGE_IMMEDIATE:
				case ASEBA_BYTECODE_LOAD_INDIRECT:
				case ASEBA_BYTECODE_STORE_INDIRECT:
				case ASEBA_BYTECODE_CONDITIONAL_BRANCH:
					pc += 2;
				break;
				
				case ASEBA_BYTECODE_EMIT:
					pc += 3;
				break;
				
				default:
					pc += 1;
				break;
			}
		}
		
		// check size
		return bytecode.size() <= targetDescription->bytecodeSize;
	}
	
	//! Get the map of event addresses to identifiers
	BytecodeVector::EventAddressesToIdsMap BytecodeVector::getEventAddressesToIds() const
	{
		EventAddressesToIdsMap eventAddr;
		const unsigned eventVectSize = (*this)[0];
		unsigned pc = 1;
		while (pc < eventVectSize)
		{
			eventAddr[(*this)[pc + 1]] = (*this)[pc];
			pc += 2;
		}
		return eventAddr;
	}
	
	//! Disassemble a microcontroller bytecode and dump it
	void Compiler::disassemble(BytecodeVector& bytecode, const PreLinkBytecode& preLinkBytecode, std::wostream& dump) const
	{
		// address of threads and subroutines
		const BytecodeVector::EventAddressesToIdsMap eventAddr(bytecode.getEventAddressesToIds());
		std::map<unsigned, unsigned> subroutinesAddr;
		
		// build subroutine map
		for (size_t id = 0; id < subroutineTable.size(); ++id)
			subroutinesAddr[subroutineTable[id].address] = id;
		
		// event table
		const unsigned eventCount = eventAddr.size();
		const float fillPrecentage = float(bytecode.size() * 100.f) / float(targetDescription->bytecodeSize);
		dump << "Disassembling " << eventCount + subroutineTable.size() << " segments (" << bytecode.size() << " words on " << targetDescription->bytecodeSize << ", " << fillPrecentage << "% filled):\n";
		
		// bytecode
		unsigned pc = eventCount*2 + 1;
		while (pc < bytecode.size())
		{
			if (eventAddr.find(pc) != eventAddr.end())
			{
				const unsigned eventId = eventAddr.at(pc);
				if (eventId == ASEBA_EVENT_INIT)
					dump << "init:       ";
				else
				{
					if (eventId < 0x1000)
					{
						if (eventId < commonDefinitions->events.size())
							dump << "event " << commonDefinitions->events[eventId].name << ": ";
						else
							dump << "unknown global event " << eventId << ": ";
					}
					else
					{
						int index = ASEBA_EVENT_LOCAL_EVENTS_START - eventId;
						if (index < (int)targetDescription->localEvents.size())
							dump << "event " << targetDescription->localEvents[index].name << ": ";
						else
							dump << "unknown local event " << index << ": ";
					}
				}
				
				PreLinkBytecode::EventsBytecode::const_iterator it = preLinkBytecode.events.find(eventId);
				assert(it != preLinkBytecode.events.end());
				dump << " (max stack " << it->second.maxStackDepth << ")\n";
			}
			
			if (subroutinesAddr.find(pc) != subroutinesAddr.end())
			{
				PreLinkBytecode::EventsBytecode::const_iterator it = preLinkBytecode.subroutines.find(subroutinesAddr[pc]);
				assert(it != preLinkBytecode.subroutines.end());
				dump << "sub " << subroutineTable[subroutinesAddr[pc]].name << ": (max stack " << it->second.maxStackDepth << ")\n";
			}
			
			dump << "    ";
			dump << pc << " (" << bytecode[pc].line << ") : ";
			switch (bytecode[pc] >> 12)
			{
				case ASEBA_BYTECODE_STOP:
				dump << "STOP\n";
				pc++;
				break;
				
				case ASEBA_BYTECODE_SMALL_IMMEDIATE:
				dump << "SMALL_IMMEDIATE " << ((signed short)(bytecode[pc] << 4) >> 4) << "\n";
				pc++;
				break;
				
				case ASEBA_BYTECODE_LARGE_IMMEDIATE:
				dump << "LARGE_IMMEDIATE " << ((signed short)bytecode[pc+1]) << "\n";
				pc += 2;
				break;
				
				case ASEBA_BYTECODE_LOAD:
				dump << "LOAD " << (bytecode[pc] & 0x0fff) << "\n";
				pc++;
				break;
				
				case ASEBA_BYTECODE_STORE:
				dump << "STORE " << (bytecode[pc] & 0x0fff) << "\n";
				pc++;
				break;
				
				case ASEBA_BYTECODE_LOAD_INDIRECT:
				dump << "LOAD_INDIRECT in array at " << (bytecode[pc] & 0x0fff) << " of size " << bytecode[pc+1] << "\n";
				pc += 2;
				break;
				
				case ASEBA_BYTECODE_STORE_INDIRECT:
				dump << "STORE_INDIRECT in array at " << (bytecode[pc] & 0x0fff) << " of size " << bytecode[pc+1] << "\n";
				pc += 2;
				break;
				
				case ASEBA_BYTECODE_UNARY_ARITHMETIC:
				dump << "UNARY_ARITHMETIC ";
				dump << unaryOperatorToString((AsebaUnaryOperator)(bytecode[pc] & ASEBA_UNARY_OPERATOR_MASK)) << "\n";
				pc++;
				break;
				
				case ASEBA_BYTECODE_BINARY_ARITHMETIC:
				dump << "BINARY_ARITHMETIC ";
				dump << binaryOperatorToString((AsebaBinaryOperator)(bytecode[pc] & ASEBA_BINARY_OPERATOR_MASK)) << "\n";
				pc++;
				break;
				
				case ASEBA_BYTECODE_JUMP:
				dump << "JUMP " << ((signed short)(bytecode[pc] << 4) >> 4) << "\n";
				pc++;
				break;
				
				case ASEBA_BYTECODE_CONDITIONAL_BRANCH:
				dump << "CONDITIONAL_BRANCH ";
				dump << binaryOperatorToString((AsebaBinaryOperator)(bytecode[pc] & ASEBA_BINARY_OPERATOR_MASK));
				if (bytecode[pc] & (1 << ASEBA_IF_IS_WHEN_BIT))
					dump << " (edge), ";
				else
					dump << ", ";
				dump << "skip " << ((signed short)bytecode[pc+1]) << " if false" << "\n";
				pc += 2;
				break;
				
				case ASEBA_BYTECODE_EMIT:
				{
					unsigned eventId = (bytecode[pc] & 0x0fff);
					dump << "EMIT ";
					if (eventId < commonDefinitions->events.size())
						dump << commonDefinitions->events[eventId].name;
					else
						dump << eventId;
					dump << " addr " << bytecode[pc+1] << " size " << bytecode[pc+2] << "\n";
					pc += 3;
				}
				break;
				
				case ASEBA_BYTECODE_NATIVE_CALL:
				dump << "CALL " << (bytecode[pc] & 0x0fff) << "\n";
				pc++;
				break;
				
				case ASEBA_BYTECODE_SUB_CALL:
				{
					unsigned address = (bytecode[pc] & 0x0fff);
					std::wstring name(L"unknown");
					for (size_t i = 0; i < subroutineTable.size(); i++)
						if (subroutineTable[i].address == address)
							name = subroutineTable[i].name;
					dump << "SUB_CALL to " << name << " @ " << address << "\n";
					pc++;
				}
				break;
				
				case ASEBA_BYTECODE_SUB_RET:
				dump << "SUB_RET\n";
				pc++;
				break;
				
				default:
				dump << "?\n";
				pc++;
				break;
			}
		}
	}
	
	/*@}*/
	
} // Aseba
