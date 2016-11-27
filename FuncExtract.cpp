#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Dwarf.h"
#include "llvm/Pass.h"
#include "llvm/Analysis/RegionPass.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <deque>
#include <string>
#include <limits>



using namespace llvm;

static cl::opt<std::string> BBListFilename("bblist", 
	   		cl::desc("List of blocks' labels that are to be extracted. Must form a valid region."), 
	   		cl::value_desc("filename"), cl::Required  );

namespace {

	typedef std::pair<unsigned,unsigned> RegionLoc;
	typedef DenseMap<Value *, DILocalVariable *> VariableDbgInfo;

	static RegionLoc getRegionLoc(const Region *);
	static RegionLoc getFunctionLoc(const Function *);
	static void getVariableDebugInfo(Function *, DenseMap<Value *, DILocalVariable *>&);
	static bool variableDeclaredInRegion(Value *, const RegionLoc&, const VariableDbgInfo&);


	// various XML helper functions as we are saving all the extracted info
	// in XML-like format. Better than self-improvised markup.  
	static std::string XMLOpeningTag(const char *key) {
		std::stringstream stream;
		stream << "<" << key << ">" << std::endl;
		return stream.str();
	}

	static std::string XMLClosingTag(const char *key) {
		std::stringstream stream;
		stream << "</" << key << ">" << std::endl;
		return stream.str();
	}

	template <typename T>
	static std::string XMLElement(const char *key, T value) {
		std::stringstream stream;
		stream << "<" << key << ">" << value << "</" << key << ">" << std::endl;
		return stream.str();
	}


	static std::pair<unsigned,unsigned> getRegionLoc(const Region *R) {
		unsigned min = std::numeric_limits<unsigned>::max();
		unsigned max = std::numeric_limits<unsigned>::min();
		
		for (auto blockIter = R->block_begin(); blockIter != R->block_end(); ++blockIter) 
		for (auto instrIter = (*blockIter)->begin(); instrIter != (*blockIter)->end(); ++instrIter) {
			const DebugLoc& x = instrIter->getDebugLoc(); 
			if (x) {
				min = std::min(min, x.getLine());
				max = std::max(max, x.getLine());
			}
		}

		return std::pair<unsigned,unsigned>(min, max);
	}


	static RegionLoc getFunctionLoc(const Function *F) {
		if ( !F->hasMetadata() || !isa<DISubprogram>(F->getMetadata(0)) ) { 
			errs() << "bad debug meta\n";
			return RegionLoc(-1, -1); 
		}

		Metadata *M = F->getMetadata(0);
		unsigned min = cast<DISubprogram>(M)->getLine(); 
		unsigned max = std::numeric_limits<unsigned>::min();

		for (auto blockIter = F->begin(); blockIter != F->end(); ++blockIter) 
		for (auto instrIter = (*blockIter).begin(); instrIter != (*blockIter).end(); ++instrIter) {
			const DebugLoc& x = instrIter->getDebugLoc();
			if (x) { 
				min = std::min(min, x.getLine());
				max = std::max(max, x.getLine()); 
			}
		}
			
		return RegionLoc(min, max); 
	}

	
	static void readBBListFile(StringMap<DenseSet<StringRef>>& F, 
										  const std::string filename) {
		std::ifstream stream;
		stream.open(filename);

		std::string tempstr;
		StringRef   current;

		while (!stream.eof()) { 
			std::getline(stream, tempstr);
			if (tempstr.length() == 0) { continue; }

			// for some reason i cannot store std::string inside llvm data structures (doesn't compile)
			// so i have to create stringrefs....
			// string does not have to be null-terminated for stringref to work
			char *buf = new char[tempstr.length()];
			std::memcpy(buf, tempstr.c_str(), tempstr.length()); 
			StringRef str(buf, tempstr.length());
			str = str.trim();

			if (tempstr.find("!") == 0) {
				str = str.ltrim("!");
				std::pair<StringRef, DenseSet<StringRef>> kv(str, DenseSet<StringRef>());
				F.insert(kv);
				current = str;
			} else {
				auto it = F.find(current);
				if (it == F.end()) { 
					errs() << "Found basic block without parent\n"; 
					continue; 
				}
				(*it).getValue().insert(str);
			}
		}

		stream.close();
	}

	//@return returns true if the current region is the one we are looking for.
	// we are expecting the first label to be the function name prefixed with ! symbol.
	static bool isTargetRegion(const Region *R, const StringMap<DenseSet<StringRef>>& regionlabels) {
		Function *F = R->getEntry()->getParent();
		auto fnit = regionlabels.find(F->getName());
		if (fnit == regionlabels.end()) { return false; }
		const DenseSet<StringRef>& blocks = fnit->getValue();

		int numblocks = 0;
		for (auto it = R->block_begin(); it != R->block_end(); ++it) {
			if (blocks.find(it->getName()) == blocks.end()) { return false; }
			numblocks++;
		}

		return (numblocks == (int)blocks.size());
	}

	// returns true if region's first basic block is also a first basic block of a function 
	static bool includesEntryBasicBlock(const Region *R) {
		Function *F = R->getEntry()->getParent();
		const BasicBlock *a = &F->getEntryBlock();
		const BasicBlock *b =  R->getEntry();
		return (a == b);
	}


	static DenseSet<Instruction *> DFSInstruction(Instruction *I) {
		DenseSet<Instruction *> visited; 

		std::deque<Instruction *> stack;
		stack.push_back(I);

		while (stack.size() != 0) {
			Instruction *current = stack.back();
			stack.pop_back();
			if (visited.find(current) != visited.end()) { continue; }
			visited.insert(current);

			for (auto it = current->op_begin(); it != current->op_end(); ++it) {
				if (auto instr = dyn_cast<Instruction>(*it)) {
					stack.push_back(instr);	
				}
			}

		}


		for (Instruction *inst: visited) {
			if (!isa<AllocaInst>(inst)) { visited.erase(inst); }

		}

		return visited;
	}



	
	//helper methods + DFS routine for finding all reachable blocks starting at some 
	//basic block BB. 
	typedef void (EnqueueBlockFunc)(std::deque<BasicBlock *>&, BasicBlock *);

	static void pushSuccessors(std::deque<BasicBlock *>& stack, BasicBlock *BB) {
		for (auto it = succ_begin(BB); it != succ_end(BB); ++it) { stack.push_back(*it); }
	}

	static void pushPredecessors(std::deque<BasicBlock *>& stack, BasicBlock *BB) {
		for (auto it = pred_begin(BB); it != pred_end(BB); ++it) { stack.push_back(*it); }
	}

	static DenseSet<BasicBlock *> DFSBasicBlocks(BasicBlock *BB, EnqueueBlockFunc enqueueFunc) {
		DenseSet<BasicBlock *> visited; 
		
		std::deque<BasicBlock *> stack;
		stack.push_back(BB);
		
		while (stack.size() != 0) { 
			BasicBlock *current = stack.front();
			stack.pop_front();
			// pick the block and expand it. If it has been visited before, we do not expand it
			if (visited.find(current) != visited.end()) { continue; }
			visited.insert(current);
			enqueueFunc(stack, current);
		}
		
		return visited;
	}

	// after DFSBasicBlocks routine we might end up having basic blocks belonging to a region 
	// in the returned basic block set. We want to remove those. 
	static void removeOwnBlocks(DenseSet<BasicBlock *>& blocks, Region *R) {
		for (auto it = R->block_begin(); it != R->block_end(); ++it) { blocks.erase(*it); }
	}

	static void analyzeFunctionArguments(Region *R, 
										 DenseSet<Value *>& inputargs) {
		Function *F = R->getEntry()->getParent();
		for (auto it = F->arg_begin(); it != F->arg_end(); ++it) {
			inputargs.insert(&*it);
		}
	}


	static void analyzeOperands(Instruction *I, 
								const DenseSet<BasicBlock *>& predecessors,
								const DenseSet<BasicBlock *>& successors,
								DenseSet<Value *>& inputargs, 
								DenseSet<Value *>& outputargs,
								const std::pair<unsigned,unsigned>& regionBounds,
								const DenseMap<Value *, DILocalVariable *>& debugInfo) {
		static DenseSet<Value *> analyzed; 
		DenseSet<Instruction*> sources = DFSInstruction(I);	
		for (auto it = sources.begin(); it != sources.end(); ++it) {
			// we don't have to look at values we have seen before... 
			if (analyzed.find(*it) != analyzed.end()) { continue; }
			analyzed.insert(*it);

			if (auto instr = dyn_cast<AllocaInst>(*it)) {
				// first we check if source instruction is allocated outside the region,
				// in one of the predecessor basic blocks. We do not care if the instruction 
				// is actually used (stored into, etc), if we did that would cause problems 
				// for stack-allocated arrays as those can be uninitialized.
				if (predecessors.find(instr->getParent()) != predecessors.end()) {
					if (!variableDeclaredInRegion(instr, regionBounds, debugInfo)) 
						inputargs.insert(instr);
				} 
				// if instruction is used by some instruction is successor basic block, we add 
				// it to the output argument list only if I is store, i.e. we modify it. 
				for (auto userit = instr->user_begin(); userit != instr->user_end(); ++userit) {
					Instruction *userinstr = cast<Instruction>(*userit);	
					BasicBlock *parentBB = userinstr->getParent(); 
					if (isa<MemCpyInst>(I) && successors.find(parentBB) != successors.end()) {
						if (variableDeclaredInRegion(instr, regionBounds, debugInfo)) 
						outputargs.insert(instr);
					}
					if (isa<StoreInst>(I) && successors.find(parentBB) != successors.end()) {
						if (variableDeclaredInRegion(instr, regionBounds, debugInfo)) 
							outputargs.insert(instr); 
					}
				}
			}
		}
	}

	// extracts debug metadata for every local variable and stores it in the map. We need this
	// to determine where variables were originally declared.
	static void getVariableDebugInfo(Function *F, DenseMap<Value *, DILocalVariable *>& map) {
		for (BasicBlock& BB : F->getBasicBlockList()) 
		for (Instruction& I :   (&BB)->getInstList()) {
			if (!isa<AllocaInst>(&I)) 
				continue; 

			if (auto *LSM = LocalAsMetadata::getIfExists(&I)) 
			if (auto *MDV = MetadataAsValue::getIfExists((&I)->getContext(), LSM)) {
				for (User *U : MDV->users()) {
					if (DbgDeclareInst *DDI = dyn_cast<DbgDeclareInst>(U)) {
						DILocalVariable *local = DDI->getVariable();
						std::pair<Value *, DILocalVariable *> kv(&I, local);
						map.insert(kv);
					}
				}
			}
		}
	}


	static bool variableDeclaredInRegion(Value *V, const std::pair<unsigned,unsigned>& regionBounds, 
										 const DenseMap<Value *, DILocalVariable *>& debugInfo) { 
		auto iterator = debugInfo.find(V);
		if (iterator == debugInfo.end()) {
			errs() << "No debug info for variable: \n";
			V->dump();
			return false;

		}
		DILocalVariable *DLV = iterator->getSecond();
		unsigned line = DLV->getLine();
		return (regionBounds.first <= line && line <= regionBounds.second);
	}


	static std::pair<DIType *, int>  getBaseType(DIType *T) {
		int ptrcount = 0;
		Metadata *md = T;	

restart:
		if (auto *a = dyn_cast<DIBasicType>(md)) { 
			goto retlabel; 
		}

		// have to look for more concrete type if we are dealing with arrays.
		if (auto *a = dyn_cast<DICompositeType>(md)) { 
			switch (a->getTag()) {
			case dwarf::DW_TAG_array_type:
				md = a->getBaseType(); ptrcount++;	
				goto restart;
			default:
				goto retlabel;
			}
		}

		// have to look for more concrete type if we are dealing with pointers.
		if (auto *a = dyn_cast<DIDerivedType>(md)) {
			switch (a->getTag()) {
			case dwarf::DW_TAG_pointer_type:
				md = a->getBaseType(); ptrcount++;	
				goto restart;
			default:
				goto retlabel;
			}
		}

retlabel:
		return std::pair<DIType *, int>(cast<DIType>(md), ptrcount);
	}


	static void writeValueInfo(Value *V, const VariableDbgInfo& VDI, bool isOutputVar, std::ofstream& out) {
		auto iterator = VDI.find(V); 
		if (iterator == VDI.end()) {
			V->dump();
			errs() << "Unknown variable, skipping...";
			return;
		}

		DILocalVariable *LV =  iterator->getSecond();
		DIType *T = dyn_cast<DIType>(iterator->getSecond()->getRawType());

		out << XMLOpeningTag("variable");
		if (isOutputVar) { out << XMLElement("isoutput", true); }


		// this isn't supposed to happen.
		if (!T) { out << XMLElement("type", "unknown"); }

		auto TT = getBaseType(T);
		out << XMLElement("name", LV->getName().str());
		out << XMLElement("ptrl", TT.second);


		if (auto *a = dyn_cast<DIBasicType>(TT.first)) {
			out << XMLElement("type", a->getName().str());
		}

		if (auto *a = dyn_cast<DICompositeType>(TT.first)) {
			if (a->getTag() == dwarf::DW_TAG_structure_type) { 
				std::stringstream stream; 
				stream << "struct " << a->getName().str();
				out << XMLElement("type", stream.str());
			}

			if (a->getTag() == dwarf::DW_TAG_array_type) {
				out << XMLElement("type", a->getName().str());
			}
		}

		if (auto *a = dyn_cast<DIDerivedType>(TT.first)) {
			if (a->getTag() == dwarf::DW_TAG_typedef) { 
				out << XMLElement("type", a->getName().str());
			}
		}

		out << XMLClosingTag("variable");
	}

	static void writeLocInfo(RegionLoc& loc, const char *tag, std::ofstream& out) {
		out << XMLOpeningTag(tag); 
		out << XMLElement("start", loc.first);
		out << XMLElement("end",   loc.second);
		out << XMLClosingTag(tag); 
	}
										 
	struct FuncExtract : public RegionPass {
		static char ID;
		FuncExtract() : RegionPass(ID) {  }
		StringMap<DenseSet<StringRef>> funcs;


// find successors / predecessors
// for each instruction in region 
// if instruction has users in successors -> output arg
// for each operand in instruction 
// if operand has users in  predecessors -> input arg
// if operand has users in successors -> output arg
		
		bool runOnRegion(Region *R, RGPassManager &RGM) override {
			if (!isTargetRegion(R, funcs)) { return false; }

			errs() << "Found region!\n";
			

			Function *F = R->getEntry()->getParent();
			std::pair<unsigned,unsigned> regionBounds = getRegionLoc(R);
			RegionLoc functionBounds = getFunctionLoc(F);

			BasicBlock *b = R->getEntry();
			DenseSet<BasicBlock *> predecessors = DFSBasicBlocks(b, pushPredecessors); 
			removeOwnBlocks(predecessors, R);

			DenseSet<BasicBlock *> successors = DFSBasicBlocks(b, pushSuccessors);
			removeOwnBlocks(successors, R);


			DenseSet<Value *> inputargs;
			DenseSet<Value *> outputargs;
			DenseMap<Value *, DILocalVariable *> debugInfo;

			getVariableDebugInfo(F, debugInfo);

			for (auto blockit = R->block_begin(); blockit != R->block_end(); ++blockit) 
			for (auto instrit = blockit->begin(); instrit != blockit->end(); ++instrit) {
				Instruction *I = &*instrit;
				if (!isa<StoreInst>(I) && !isa<LoadInst>(I) && !isa<MemCpyInst>(I)) { continue; }
				analyzeOperands(I, predecessors, successors, inputargs, outputargs, regionBounds, debugInfo );
			}

			std::ofstream outfile;
			outfile.open("extractinfo.txt", std::ofstream::out);
			//writing stuff in xml-like format
			outfile << XMLOpeningTag("extractinfo");
			//writeFileInfo(regionBounds, functionBounds, outfile);
			writeLocInfo(regionBounds, "region", outfile);
			writeLocInfo(functionBounds, "function", outfile);

			// dump variable info...
			for (Value *V : inputargs)  { writeValueInfo(V, debugInfo, false, outfile); }
			for (Value *V : outputargs) { writeValueInfo(V, debugInfo, true,  outfile); }
			outfile << XMLClosingTag("extractinfo");
			outfile.close();


#if 0
			if (includesEntryBasicBlock(R)) {
				//analyzeFunctionArguments(R, inputargs);
			}
#endif
			

			return false;
		}


		bool doInitialization(Region *R, RGPassManager &RGM) override {
			//TODO we should probably initialize this in the constructor?
			static bool read = false;
			if (read != 0) { return false; }
			read = 1;
			readBBListFile(funcs, BBListFilename);
			return false;	
		}

		bool doFinalization(void) override {
			//errs() << "Cleaning up!\n";
			// TODO delete buffers used by StringRefs
			for (auto it = funcs.begin(); it != funcs.end(); ++it) {
				//delete[] (*it).first().data();	
				DenseSet<StringRef>& set = it->getValue();
				for (auto b = set.begin(); b != set.end(); ++b) {
					//delete[] b->data();
				}

			}
			return false;	
		}

		~FuncExtract(void) {
			errs() << "Hello I am your destructor!\n";
		}

	};
}

char FuncExtract::ID = 0;
static RegisterPass<FuncExtract> X("funcextract", "Func Extract", true, true);
