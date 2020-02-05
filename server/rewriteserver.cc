
#include "codegenerator.h"
#include "config.h"
#include "connection.h"
#include "optimizer.h"

#include <rellume/rellume.h>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassTimingInfo.h>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <sstream>


#define SPTR_ADDR_SPACE 1

static LLDecodeStop opt_decode_stop = RELLUME_DECODE_ALL;

struct StructOff {
    struct Entry {
        unsigned off;
        unsigned size;
        llvm::Value* PtrTo(llvm::IRBuilder<>& irb, llvm::Value* base) const {
            llvm::Type* ty = irb.getIntNTy(8*size)->getPointerTo(SPTR_ADDR_SPACE);
            return irb.CreatePointerCast(irb.CreateConstGEP1_64(base, off), ty);
        }
    };

#define RELLUME_PUBLIC_REG(name,nameu,sz,off) \
            inline static constexpr Entry nameu = Entry{ off, sz };
#include <rellume/cpustruct.inc>
#undef RELLUME_PUBLIC_REG
};

static llvm::Function* CreateFunc(llvm::Module* mod, const std::string name,
                                  bool external = true) {
    llvm::LLVMContext& ctx = mod->getContext();
    llvm::Type* void_type = llvm::Type::getVoidTy(ctx);
    llvm::Type* i8p_type = llvm::Type::getInt8PtrTy(ctx, SPTR_ADDR_SPACE);
    auto fn_ty = llvm::FunctionType::get(void_type, {i8p_type}, false);
    auto linkage = external ? llvm::GlobalValue::ExternalLinkage
                            : llvm::GlobalValue::PrivateLinkage;
    return llvm::Function::Create(fn_ty, linkage, name, mod);
}

template<typename F>
static llvm::Function* CreateFuncImpl(llvm::Module* mod, const std::string name,
                                      const F& f) {
    llvm::Function* fn = CreateFunc(mod, name, /*external=*/false);
    fn->addFnAttr(llvm::Attribute::AlwaysInline);

    llvm::BasicBlock* bb = llvm::BasicBlock::Create(mod->getContext(), "", fn);
    llvm::IRBuilder<> irb(bb);
    f(irb, fn->arg_begin());
    return fn;
}

static llvm::Function* CreateNoopFn(llvm::Module* mod) {
    return CreateFuncImpl(mod, "noop_stub", [](llvm::IRBuilder<>& irb, llvm::Value* arg) {
        irb.CreateRetVoid();
    });
}

static llvm::Function* CreateRdtscFn(llvm::Module* mod) {
    return CreateFuncImpl(mod, "rdtsc", [](llvm::IRBuilder<>& irb, llvm::Value* arg) {
        irb.CreateStore(irb.getInt64(0), StructOff::RAX.PtrTo(irb, arg));
        irb.CreateStore(irb.getInt64(0), StructOff::RDX.PtrTo(irb, arg));
        irb.CreateRetVoid();
    });
}

int main(int argc, char** argv) {
    if (argc > 1) {
        std::cerr << "usage: " << argv[0] << std::endl;
        std::cerr << "all configuration is done by the client." << std::endl;
        return 1;
    }

    // Set stdio to unbuffered
    std::setbuf(stdin, nullptr);
    std::setbuf(stdout, nullptr);

    // Measured times
    std::chrono::steady_clock::duration dur_lifting{};
    std::chrono::steady_clock::duration dur_llvm_opt{};
    std::chrono::steady_clock::duration dur_llvm_codegen{};

    Conn conn; // uses stdio

    ServerConfig server_config;
    if (conn.RecvMsg() != Msg::C_INIT) {
        std::cerr << "error: expected C_INIT message" << std::endl;
        return 1;
    }
    server_config.ReadFromConn(conn);

    llvm::cl::ParseEnvironmentOptions(argv[0], "INSTREW_SERVER_LLVM_OPTS");
    llvm::TimePassesIsEnabled = server_config.debug_time_passes;

    // Initialize optimizer according to configuration
    Optimizer optimizer(server_config);

    // Create code generator to write code into our buffer
    llvm::SmallVector<char, 4096> obj_buffer;
    CodeGenerator codegen(server_config, obj_buffer);

    // Create module, functions will be deleted after code generation.
    llvm::LLVMContext ctx;
    llvm::Module mod("mod", ctx);

    // Add syscall implementation to module
    auto syscall_fn = CreateFunc(&mod, "syscall");
    auto noop_fn = CreateNoopFn(&mod);
    auto cpuid_fn = CreateFunc(&mod, "cpuid");
    auto rdtsc_fn = CreateRdtscFn(&mod);

    llvm::Function* const helper_fns[] = {noop_fn, cpuid_fn, rdtsc_fn};

    // Create rellume config
    LLConfig* rlcfg = ll_config_new();
    ll_config_enable_verify_ir(rlcfg, false);
    ll_config_set_call_ret_clobber_flags(rlcfg, server_config.opt_unsafe_callret);
    ll_config_set_use_native_segment_base(rlcfg, server_config.native_segments);
    ll_config_set_hhvm(rlcfg, server_config.hhvm);
    ll_config_set_sptr_addrspace(rlcfg, SPTR_ADDR_SPACE);
    ll_config_enable_overflow_intrinsics(rlcfg, false);
    ll_config_set_instr_impl(rlcfg, LL_INS_SYSCALL, llvm::wrap(syscall_fn));
    ll_config_set_instr_impl(rlcfg, LL_INS_CPUID, llvm::wrap(cpuid_fn));
    ll_config_set_instr_impl(rlcfg, LL_INS_RDTSC, llvm::wrap(rdtsc_fn));
    ll_config_set_instr_impl(rlcfg, LL_INS_FLDCW, llvm::wrap(noop_fn));
    ll_config_set_instr_impl(rlcfg, LL_INS_LDMXCSR, llvm::wrap(noop_fn));

    auto mem_acc = [](size_t addr, uint8_t* buf, size_t buf_sz, void* user_arg) {
        Conn* conn = static_cast<Conn*>(user_arg);

        struct { uint64_t addr; size_t buf_sz; } send_buf{addr, buf_sz};
        conn->SendMsg(Msg::S_MEMREQ, send_buf);

        Msg::Id msgid = conn->RecvMsg();
        std::size_t msgsz = conn->Remaining();

        // Sanity checks.
        if (msgid != Msg::C_MEMBUF)
            return size_t{0};
        if (msgsz < 1 || msgsz > buf_sz + 1)
            return size_t{0};

        conn->Read(buf, msgsz - 1);

        uint8_t failed = conn->Read<uint8_t>();
        if (failed)
            return size_t{0};

        return msgsz - 1;
    };

    while (true) {
        Msg::Id msgid = conn.RecvMsg();
        if (msgid == Msg::C_EXIT) {
            if (server_config.debug_profile_server) {
                std::cerr << "Server profile: "
                          << std::chrono::duration_cast<std::chrono::milliseconds>(dur_lifting).count()
                          << "ms lifting; "
                          << std::chrono::duration_cast<std::chrono::milliseconds>(dur_llvm_opt).count()
                          << "ms llvm_opt; "
                          << std::chrono::duration_cast<std::chrono::milliseconds>(dur_llvm_codegen).count()
                          << "ms llvm_codegen"
                          << std::endl;
            }
            // if (server_config.debug_time_passes)
            //     llvm::reportAndResetTimings(&llvm::errs());
            return 0;
        } else if (msgid == Msg::C_TRANSLATE) {
            auto addr = conn.Read<uint64_t>();

            ////////////////////////////////////////////////////////////////////
            // STEP 1: lift function to LLVM-IR using Rellume.

            std::chrono::steady_clock::time_point time_lifting_start;
            if (server_config.debug_profile_server)
                time_lifting_start = std::chrono::steady_clock::now();

            LLFunc* rlfn = ll_func_new(llvm::wrap(&mod), rlcfg);
            ll_func_decode3(rlfn, addr, opt_decode_stop, mem_acc, &conn);
            llvm::Function* fn = llvm::unwrap<llvm::Function>(ll_func_lift(rlfn));
            ll_func_dispose(rlfn);

            std::stringstream namebuf;
            namebuf << std::hex << "func_" << addr;

            fn->setName(namebuf.str());

            if (server_config.debug_profile_server)
                dur_lifting += std::chrono::steady_clock::now() - time_lifting_start;

            // Print IR before optimizations
            if (server_config.debug_dump_ir)
                fn->print(llvm::errs());

            ////////////////////////////////////////////////////////////////////
            // STEP 2: optimize lifted LLVM-IR, optionally using the new pass
            //   manager of LLVM

            std::chrono::steady_clock::time_point time_llvm_opt_start;
            if (server_config.debug_profile_server)
                time_llvm_opt_start = std::chrono::steady_clock::now();

            // Remove unused helper functions to prevent erasure during opt.
            for (const auto& helper_fn : helper_fns)
                if (helper_fn->user_empty())
                    helper_fn->removeFromParent();

            optimizer.Optimize(fn);

            if (server_config.debug_profile_server)
                dur_llvm_opt += std::chrono::steady_clock::now() - time_llvm_opt_start;

            // Print IR before target-specific transformations
            if (server_config.debug_dump_ir)
                fn->print(llvm::errs());

            ////////////////////////////////////////////////////////////////////
            // STEP 3: generate machine code

            std::chrono::steady_clock::time_point time_llvm_codegen_start;
            if (server_config.debug_profile_server)
                time_llvm_codegen_start = std::chrono::steady_clock::now();

            codegen.GenerateCode(&mod);

            if (server_config.debug_profile_server)
                dur_llvm_codegen += std::chrono::steady_clock::now() - time_llvm_codegen_start;

            // Print IR after all optimizations are done
            if (server_config.debug_dump_ir)
                fn->print(llvm::errs());

            ////////////////////////////////////////////////////////////////////
            // STEP 4: send object file to the client, and clean-up.

            conn.SendMsg(Msg::S_OBJECT, obj_buffer.data(), obj_buffer.size());

            if (server_config.debug_dump_objects) {
                std::stringstream debug_out1_name;
                debug_out1_name << std::hex << "func_" << addr << ".elf";

                std::ofstream debug_out1;
                debug_out1.open(debug_out1_name.str(), std::ios::binary);
                debug_out1.write(obj_buffer.data(), obj_buffer.size());
                debug_out1.close();
            }

            fn->eraseFromParent();

            // Re-add helper functions removed previously
            for (const auto& helper_fn : helper_fns)
                if (!helper_fn->getParent())
                    mod.getFunctionList().push_back(helper_fn);
        } else {
            std::cerr << "unexpected msg " << msgid << std::endl;
            return 1;
        }
    }
}
