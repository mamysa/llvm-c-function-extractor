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

	static bool  				  isEarlyReturnBlock(const BasicBlock *);
	static DenseSet<BasicBlock *> findEarlyReturnsInRegion(const Region *);
	static RegionLoc getRegionLoc(DenseSet<BasicBlock *>&);
	static void getVariableDebugInfo(Function *, DenseMap<Value *, DILocalVariable *>&);
	static bool variableDeclaredInRegion(Value *, const RegionLoc&, const VariableDbgInfo&);
										 

	static std::pair<unsigned,unsigned> getRegionLoc(const Region *R) {
		unsigned min = std::numeric_limits<unsigned>::max();
		unsigned max = std::numeric_limits<unsigned>::min();
		
		for (auto blockIter = R->block_begin(); blockIter != R->block_end(); ++blockIter) 
		for (auto instrIter = (*blockIter)->begin(); instrIter != (*blockIter)->end(); ++instrIter) {
			const DebugLoc& x = instrIter->getDebugLoc(); //FIXME invalid debugloc? 
			if (x) {
				min = std::min(min, x.getLine());
				max = std::max(max, x.getLine());
			}
		}

		return std::pair<unsigned,unsigned>(min, max);
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


	static DenseSet<Value *> DFSInstruction(Instruction *I) {
		DenseSet<Value *> collected;
		DenseSet<Value *> visited; 
		std::deque<Value *> stack;
		stack.push_back(I);

		while (stack.size() != 0) {
			Value *current = stack.back();
			stack.pop_back();
			
			if (visited.find(current) != visited.end()) {
				continue;
			}

			// terminate if current instruction is alloca 
			if (auto alloca = dyn_cast<AllocaInst>(current)) {
				collected.insert(alloca);
			}

		if (auto global = dyn_cast<GlobalValue>(current)) {
				collected.insert(global);
			}

			visited.insert(current);
			// FIXME more careful selection of operands for certain instructions 
			if (auto a = dyn_cast<Instruction>(current)) {
				if (auto b = dyn_cast<StoreInst>(a)) {
					stack.push_back(b->getOperand(1));
				}
				else {

					for (auto it = a->op_begin(); it != a->op_end(); ++it) {
						if (auto instr = dyn_cast<Instruction>(*it)) {
							stack.push_back(instr);	
						}
						if (auto global = dyn_cast<GlobalValue>(*it)) {
							stack.push_back(global);	
						}
					}
				}

			}

		}

		return collected;
	}



	
	typedef void (EnqueueBlockFunc)(std::deque<BasicBlock *>, BasicBlock *);

	enum DFSDirection { SUCC, PRED };
	static DenseSet<BasicBlock *> DFS(BasicBlock *BB, enum DFSDirection d) {
		DenseSet<BasicBlock *> blocks; 
		
		std::deque<BasicBlock *> stack;
		stack.push_back(BB);
		
		while (stack.size() != 0) { 
			BasicBlock *current = stack.front();
			stack.pop_front();
			// pick the block and expand it. If it has been visited before, we do not expand it.		
			if (blocks.find(current) != blocks.end()) {
				continue; 
			}
			
			blocks.insert(current);

			switch (d) {
			case SUCC: 
				for (auto it = succ_begin(current); it != succ_end(current); ++it) {
					stack.push_back(*it);
				}
				break;
			case PRED:
				for (auto it = pred_begin(current); it != pred_end(current); ++it) {
					stack.push_back(*it);
				}
				break;
			}
		}
		
		return blocks;
	}

	static void removeOwnBlocks(DenseSet<BasicBlock *>& blocks, Region *R) {
		for (auto it = R->block_begin(); it != R->block_end(); ++it) {
			blocks.erase(*it);		
		}
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
		DenseSet<Value *> sources = DFSInstruction(I);	
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
#if 0
			if (auto global = dyn_cast<GlobalValue>(*it)) {
				// globals are added to input argument list inconditionally. 
				// if current instruction is store, we also add it to return values applying
				// the same typing rules listed above
				inputargs.insert(global); // FIXME also pushes memcpy??
				if (isa<StoreInst>(I)) {
					outputargs.insert(global);
				}
			}
#endif
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

		// start looking for something more specific if  DerivedType is either a pointer 
		// or CompositeType is an array. 
		int ptrlevel = 0;
		Metadata *md = T;	

loop:
		if (auto *a = dyn_cast<DIBasicType>(md)) { 
			goto retlabel; 
		}

		if (auto *a = dyn_cast<DICompositeType>(md)) { 
			switch (a->getTag()) {
			case dwarf::DW_TAG_array_type:
				md = a->getBaseType(); ptrlevel++;	
				goto loop;
			default:
				goto retlabel;

			}
		}

		if (auto *a = dyn_cast<DIDerivedType>(md)) {
			switch (a->getTag()) {
			case dwarf::DW_TAG_pointer_type:
				md = a->getBaseType(); ptrlevel++;	
				goto loop;
			default:
				goto retlabel;
			}
		}

retlabel:
		return std::pair<DIType *, int>(cast<DIType>(md), ptrlevel);
	}


	static void dumpValueInfo(Value *V, const VariableDbgInfo& VDI) {
		auto iterator = VDI.find(V); 
		if (iterator == VDI.end()) {
			errs() << "TYPE: UNKNOWN\n";
			V->dump();
			return;
			//TODO exit ?
		}

		DILocalVariable *LV =  iterator->getSecond();
		DIType *T = dyn_cast<DIType>(iterator->getSecond()->getRawType());

		V->dump();

		if (!T) { 
			errs() << "RawType() - unexpected metadata type\n"; 
		}

		auto TT = getBaseType(T);

		if (auto *a = dyn_cast<DIBasicType>(TT.first)) {
			errs() << "TYPE: " << a->getName() << " " << TT.second << "\n";
			errs() << "NAME: " << LV->getName() << "\n";
		}

		if (auto *a = dyn_cast<DICompositeType>(TT.first)) {
			if (a->getTag() == dwarf::DW_TAG_structure_type) { 
				errs() << "TYPE: struct " << TT.second << " "  << a->getName() <<"\n"; 
			}

			if (a->getTag() == dwarf::DW_TAG_array_type) {
				errs() << "TYPE " << a->getName() << " " << TT.second << "\n";
				errs() << "NAME: " << LV->getName() << "\n";

			}


		}

		if (auto *a = dyn_cast<DIDerivedType>(TT.first)) {
			if (a->getTag() == dwarf::DW_TAG_typedef) { 
				errs() << "TYPE: " << a->getName() << " " << TT.second << "\n"; 
				errs() << "NAME: " << LV->getName() << "\n";
			}

		}
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
			if (!isTargetRegion(R, funcs)) { 
				return false; 
			}

			errs() << "Found region!\n";

			std::pair<unsigned,unsigned> regionBounds = getRegionLoc(R);

			BasicBlock *b = R->getEntry();
			DenseSet<BasicBlock *> predecessors = DFS(b, PRED); 
			removeOwnBlocks(predecessors, R);

			DenseSet<BasicBlock *> successors = DFS(b, SUCC);
			removeOwnBlocks(successors, R);

			DenseSet<Value *> inputargs;
			DenseSet<Value *> outputargs;
			DenseMap<Value *, DILocalVariable *> debugInfo;

			Function *F = R->getEntry()->getParent();
			getVariableDebugInfo(F, debugInfo);

			for (auto blockit = R->block_begin(); blockit != R->block_end(); ++blockit) 
			for (auto instrit = blockit->begin(); instrit != blockit->end(); ++instrit) {
				Instruction *I = &*instrit;
				if (!isa<StoreInst>(I) && !isa<LoadInst>(I) && !isa<MemCpyInst>(I)) { continue; }
				analyzeOperands(I, predecessors, successors, inputargs, outputargs, regionBounds, debugInfo );
			}
			



			errs() << "\nin\n";
			for (Value *V : inputargs) {
				errs() << "-----\n";
				dumpValueInfo(V, debugInfo);
			}


			errs() << "\nout\n";
			for (Value *V : outputargs) {
				errs() << "-----\n";
				dumpValueInfo(V, debugInfo);
			}


#if 0
			if (includesEntryBasicBlock(R)) {
				//analyzeFunctionArguments(R, inputargs);
			}
#endif
			


#if 0
			for (auto it = inputargs.begin(); it != inputargs.end(); ++it) {
				auto *L = LocalAsMetadata::get(*it);
				auto *MDV = MetadataAsValue::get((*it)->getContext(), L);
				for (User *U : MDV->users()) {
					DbgDeclareInst *DDI = dyn_cast<DbgDeclareInst>(U);
					// gives us <Value *, DbgDeclareInstr *> pair
					if (DDI) {
						DILocalVariable *localVar = DDI->getVariable();
						(*it)->dump();
						errs() << localVar->getName() << "\n";
						errs() << localVar->getLine() << "\n";
						Metadata *m = localVar->getType();
						if (DIBasicType *DIBT = dyn_cast<DIBasicType>(m)) {
							errs() << DIBT->getName() << "\n";
						}
						if (DICompositeType *DICT = dyn_cast<DICompositeType>(m)) {
							if (DICT->getTag() == dwarf::DW_TAG_structure_type) {

								errs() << "struct " << DICT->getName() << "\n";
							}
						}


						if (DIDerivedType *DICT = dyn_cast<DIDerivedType>(m)) {
							if (DICT->getTag() == dwarf::DW_TAG_pointer_type) {
								errs() << "pointer " << DICT->getName() << "\n";

								int ptrcount = 1;
								//gotta reach the type that is not tagged as pointer...
								Metadata *t = DICT->getRawBaseType();
								while (true) {
									if (auto *x = dyn_cast<DIDerivedType>(t)) {
										t = x->getRawBaseType();
										ptrcount++;
										continue;
									}

									if (auto *x = dyn_cast<DICompositeType>(t)) {
										errs() << x->getName() << " " << ptrcount << "\n";
										break;
									}

									if (auto *x = dyn_cast<DIBasicType>(t)) {
										errs() << x->getName() << " " << ptrcount << "\n";
										break;
									}
								}




								if (auto t2 = dyn_cast<DIDerivedType>(t)) {
									t2->dump();
								}




								//(*it)->dump();
							}
						}

					}
				}


				errs() << "--\n";
			}
#endif
			


#if 0
			Function *F = R->getEntry()->getParent();
			for (auto bit = F->begin(); bit != F->end(); ++bit) {
				BasicBlock *bb = &*bit;
				for (auto iit = bb->begin(); iit != bb->end(); ++iit) {
					if (auto instr = dyn_cast<DbgDeclareInst>(&*iit)) {





					}
				}
			}
#endif

#if 0
				if (auto instr = dyn_cast<Instruction>(*it)) {
					analyzeMetadata(instr);
				}
			}

#endif

#if 0
			errs() << "in args: \n";
			for (auto it = inputargs.begin(); it != inputargs.end(); ++it) {
				(*it)->dump();
			}

			errs() << "out args: \n";
			for (auto it = outputargs.begin(); it != outputargs.end(); ++it) {
				(*it)->dump();
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
static RegisterPass<FuncExtract> X("funcextract", "Func Extract", false, false);
