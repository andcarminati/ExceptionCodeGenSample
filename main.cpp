// Author: Andreu Carminati
#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/ExecutionEngine/Orc/CompileUtils.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/IRCompileLayer.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <memory>

/*
This program generates LLVM IR for the code below:

void foo(){
    puts("Foo: I will throw an exception");
    throw 1;
}

void bar(){
    try    {
        puts("Bar: I will call a function that raises an exception");
        bar();
    } catch (int e){
        puts("Bar: I catched an exception");
    }
}

Then it JITs and executes the code.

*/


namespace llvm {
namespace orc {

class ExecJIT {
private:
  std::unique_ptr<ExecutionSession> ES;
  RTDyldObjectLinkingLayer ObjectLayer;
  IRCompileLayer CompileLayer;

  DataLayout DL;
  MangleAndInterner Mangle;
  ThreadSafeContext Ctx;

  JITDylib &MainJD;

public:
  ExecJIT(std::unique_ptr<ExecutionSession> ES, JITTargetMachineBuilder JTMB,
          DataLayout DL)
      : ES(std::move(ES)),
        ObjectLayer(*this->ES,
                    []() { return std::make_unique<SectionMemoryManager>(); }),
        CompileLayer(*this->ES, ObjectLayer,
                     std::make_unique<ConcurrentIRCompiler>(std::move(JTMB))),
        DL(std::move(DL)), Mangle(*this->ES, this->DL),
        Ctx(std::make_unique<LLVMContext>()),
        MainJD(this->ES->createBareJITDylib("<main>")) {
    MainJD.addGenerator(
        cantFail(DynamicLibrarySearchGenerator::GetForCurrentProcess(
            DL.getGlobalPrefix())));
  }
   ~ExecJIT() {
    if (auto Err = ES->endSession())
      ES->reportError(std::move(Err));
  }

  static Expected<std::unique_ptr<ExecJIT>> Create() {
    auto EPC = SelfExecutorProcessControl::Create();
    if (!EPC)
      return EPC.takeError();

    auto ES = std::make_unique<ExecutionSession>(std::move(*EPC));

    JITTargetMachineBuilder JTMB(
        ES->getExecutorProcessControl().getTargetTriple());

    auto DL = JTMB.getDefaultDataLayoutForTarget();
    if (!DL)
      return DL.takeError();

    return std::make_unique<ExecJIT>(std::move(ES), std::move(JTMB),
                                     std::move(*DL));
  }

  const DataLayout &getDataLayout() const { return DL; }

  LLVMContext &getContext() { return *Ctx.getContext(); }

  Error addModule(ThreadSafeModule TSM) {
    auto RT = MainJD.getDefaultResourceTracker();
    return CompileLayer.add(RT, std::move(TSM));
  }

  Expected<JITEvaluatedSymbol> lookup(StringRef Name) {
    return ES->lookup({&MainJD}, Mangle(Name.str()));
  }
};

} // end namespace orc
} // end namespace llvm

using namespace llvm;
using namespace std;

// All globals, as a prof of concept
unique_ptr<LLVMContext> TheContext;
unique_ptr<Module> TheModule;

IRBuilder<>* Builder = nullptr;

GlobalVariable *TypeInfo = nullptr;

FunctionType *AllocEHFty = nullptr;
Function *AllocEHFn = nullptr;

FunctionType *ThrowEHFty = nullptr;
Function *ThrowEHFn = nullptr;

FunctionType *ThrowFuncFty = nullptr;
Function *ThrowFuncFn = nullptr;

FunctionType *CatchFuncFty = nullptr;
Function *CatchFuncFn = nullptr;

FunctionType *PersFty = nullptr;
Function *PersFn = nullptr;

BasicBlock *LPadBB = nullptr;
BasicBlock *UnreachableBB = nullptr;

FunctionType *TypeIdFty = nullptr;
Function *TypeIdFn = nullptr;

FunctionType *BeginCatchFty = nullptr;
Function *BeginCatchFn = nullptr;

FunctionType *EndCatchFty = nullptr;
Function *EndCatchFn = nullptr;

FunctionType *PutsFty = nullptr;
Function *PutsFn = nullptr;

Type *Int8PtrTy = nullptr;
Type *Int32Ty = nullptr;
Type *Int32PtrTy = nullptr;
Type *Int64Ty = nullptr;
Type *VoidTy = nullptr;

void addLandingPad();

void createFunc(FunctionType *&Fty, Function *&Fn, const Twine &N, Type *Result,
                ArrayRef<Type *> Params = None, bool IsVarArgs = false) {
  Fty = FunctionType::get(Result, Params, IsVarArgs);
  Fn = Function::Create(Fty, Function::ExternalLinkage, N, *TheModule);
}

void createDecls() {
  TypeInfo =
      new GlobalVariable(*TheModule, Type::getInt8PtrTy(*TheContext),
                         /*isConstant=*/true, GlobalValue::ExternalLinkage,
                         /*Initializer=*/nullptr, "_ZTIi");

  createFunc(AllocEHFty, AllocEHFn, "__cxa_allocate_exception", Int8PtrTy,
             {Int64Ty});
  createFunc(ThrowEHFty, ThrowEHFn, "__cxa_throw", VoidTy,
             {Int8PtrTy, Int8PtrTy, Int8PtrTy});
  createFunc(TypeIdFty, TypeIdFn, "llvm.eh.typeid.for", Int32Ty, {Int8PtrTy});
  createFunc(BeginCatchFty, BeginCatchFn, "__cxa_begin_catch", Int8PtrTy,
             {Int8PtrTy});
  createFunc(EndCatchFty, EndCatchFn, "__cxa_end_catch", VoidTy);
  createFunc(PutsFty, PutsFn, "puts", Int32Ty, {Int8PtrTy});
  createFunc(CatchFuncFty, CatchFuncFn, "bar", VoidTy);;
  createFunc(PersFty, PersFn, "__gxx_personality_v0", Int32Ty, None, true);
}

void createThrowFunc() {

  createFunc(ThrowFuncFty, ThrowFuncFn, "foo", VoidTy);
  ThrowFuncFn->setDSOLocal(true);

  auto *Entry = BasicBlock::Create(*TheContext, "entry", ThrowFuncFn);
  Builder->SetInsertPoint(Entry);

  Constant *PayloadSz = ConstantInt::get(Int64Ty, 4, false);
  CallInst *EH = Builder->CreateCall(
      AllocEHFty, AllocEHFn, {PayloadSz}, "exception");

  Value *PayloadPtr = Builder->CreateBitCast(EH, Int32PtrTy);
  Builder->CreateAlignedStore(ConstantInt::get(Int32Ty, 1, true), PayloadPtr,
                             MaybeAlign(16));

  Value *MsgPtr = Builder->CreateGlobalStringPtr(
                "Foo: I will throw an exception", "msg", 0, TheModule.get());
  Builder->CreateCall(PutsFty, PutsFn, {MsgPtr});              
                
  Builder->CreateCall(
      ThrowEHFty, ThrowEHFn,
      {EH, ConstantExpr::getBitCast(TypeInfo, Int8PtrTy),
       ConstantPointerNull::get(PointerType::getInt8PtrTy(*TheContext))});
  Builder->CreateUnreachable();
}

void createCatchFunc() {
 
  CatchFuncFn->setPersonalityFn(PersFn);

  auto *BB = BasicBlock::Create(*TheContext, "entry", CatchFuncFn);
  auto *Lpad = BasicBlock::Create(*TheContext, "lpad", CatchFuncFn);
  auto *CatchDispatch =
      BasicBlock::Create(*TheContext, "catch.dispatch", CatchFuncFn);
  auto *Catch = BasicBlock::Create(*TheContext, "catch", CatchFuncFn);
  auto *Icont = BasicBlock::Create(*TheContext, "invoke.cont", CatchFuncFn);
  auto *Trycont = BasicBlock::Create(*TheContext, "try.cont", CatchFuncFn);
  auto *Resume = BasicBlock::Create(*TheContext, "eh.resume", CatchFuncFn);

  Builder->SetInsertPoint(BB);
  auto e = Builder->CreateAlloca(Int32Ty, 0, "e");
  auto exnSlot = Builder->CreateAlloca(Int8PtrTy, 0, "ext.slot");
  auto ehSelSlot = Builder->CreateAlloca(Int32Ty, 0, "ehselector.slot");

  Value *MsgPtr = Builder->CreateGlobalStringPtr(
                "Bar: I will call a function that raises an exception", "msg", 0, TheModule.get());
  Builder->CreateCall(PutsFty, PutsFn, {MsgPtr});


  Builder->CreateInvoke(ThrowFuncFty, ThrowFuncFn, Icont, Lpad, {});

  Builder->SetInsertPoint(Lpad);
  LandingPadInst *Exc =
      Builder->CreateLandingPad(StructType::get(Int8PtrTy, Int32Ty), 1, "exc");
  Exc->addClause(ConstantExpr::getBitCast(TypeInfo, Int8PtrTy));

  auto Sel = Builder->CreateExtractValue(Exc, {0}, "exc.sel");
  Builder->CreateStore(Sel, exnSlot);

  auto Slot = Builder->CreateExtractValue(Exc, {1}, "exc.slod");
  Builder->CreateStore(Slot, ehSelSlot);

  Builder->CreateBr(CatchDispatch);

  Builder->SetInsertPoint(CatchDispatch);
  auto Sel1 = Builder->CreateLoad(Int32Ty, ehSelSlot, "sel");
  auto TypeID = Builder->CreateCall(
      TypeIdFty, TypeIdFn, {ConstantExpr::getBitCast(TypeInfo, Int8PtrTy)});
  auto Matches = Builder->CreateCmp(CmpInst::ICMP_EQ, Sel1, TypeID, "matches");
  Builder->CreateCondBr(Matches, Catch, Resume);

  Builder->SetInsertPoint(Catch);
  auto Exn = Builder->CreateLoad(Int8PtrTy, exnSlot, "exn");
  auto CallBeginCatch = Builder->CreateCall(BeginCatchFty, BeginCatchFn, {Exn});
  auto ExceptValuePrt = Builder->CreateBitCast(CallBeginCatch, Int32PtrTy);
  auto ExceptValue = Builder->CreateLoad(Int32Ty, ExceptValuePrt);
  Builder->CreateStore(ExceptValue, e);
  Value *MsgPtr2 = Builder->CreateGlobalStringPtr(
                "Bar: I catched an exception", "msg", 0, TheModule.get());
  Builder->CreateCall(PutsFty, PutsFn, {MsgPtr2});
  auto CallEndCatch = Builder->CreateCall(EndCatchFty, EndCatchFn, {});
  Builder->CreateBr(Trycont);


  Builder->SetInsertPoint(Icont);
  Builder->CreateBr(Trycont);

  Builder->SetInsertPoint(Trycont);
  Builder->CreateRetVoid();

  Builder->SetInsertPoint(Resume);
  auto Exn2 = Builder->CreateLoad(Int8PtrTy, exnSlot, "exn2");
  auto Sel2 = Builder->CreateLoad(Int32Ty, ehSelSlot, "exn2");

  auto LpadVal =
      Builder->CreateInsertValue(UndefValue::get(Exc->getType()), Exn2, 0);
  auto LpadVal2 = Builder->CreateInsertValue(LpadVal, Sel2, 1);
  Builder->CreateResume(LpadVal2);
}

static ExitOnError ExitOnErr;

void execute() {
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();


  std::unique_ptr<llvm::orc::ExecJIT> TheJIT = ExitOnErr(llvm::orc::ExecJIT::Create());
  TheModule->setDataLayout(TheJIT->getDataLayout());
  ExitOnErr(TheJIT->addModule(
      llvm::orc::ThreadSafeModule(
          std::move(TheModule), std::move(TheContext))));

    // Search the JIT for the __anon_expr symbol.
    auto ExprSymbol = ExitOnErr(TheJIT->lookup("bar"));
    
    // necessary???
    if(!ExprSymbol){
        llvm::errs() << "bar function not found\n";
        return;
    }

    // Get the symbol's address and cast it to the right type (takes no
    // arguments, returns a double) so we can call it as a native function.
    auto *FP = (void (*)())(intptr_t) ExprSymbol.getAddress();
    assert(FP && "Failed to codegen function");
    FP();
}

int main(int argc, char const *argv[]) {

  TheContext = std::make_unique<LLVMContext>();
  TheModule = std::make_unique<Module>("jit", *TheContext);
  Builder = new IRBuilder<>(*TheContext);
  Int8PtrTy = Type::getInt8PtrTy(*TheContext);
  Int32PtrTy = Type::getInt32PtrTy(*TheContext);
  Int32Ty = Type::getInt32Ty(*TheContext);
  Int64Ty = Type::getInt64Ty(*TheContext);
  VoidTy = Type::getVoidTy(*TheContext);

  createDecls();
  createThrowFunc();
  createCatchFunc();
  execute();
  
  return 0;
}
