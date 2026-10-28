/*
 * PROJECT:     ReactOS SDK
 * LICENSE:     BSD Zero Clause License (https://spdx.org/licenses/0BSD)
 * PURPOSE:     Helper pragma implementation for pseh library (amd64)
 * COPYRIGHT:   Copyright 2021 Jérôme Gardou
 *              Copyright 2024 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

#include <gcc-plugin.h>
#include <plugin-version.h>
#include <function.h>
#include <tree.h>
#include <c-family/c-pragma.h>
#include <c-family/c-common.h>
#include <tree-pass.h>
#include <context.h>
#include <memmodel.h>
#include <rtl.h>
#include <emit-rtl.h>
#include <diagnostic-core.h>

#include <iostream>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <cstdio>

#if 0 // To enable tracing
#define trace(...) fprintf(stderr, __VA_ARGS__)
#else
#define trace(...)
#endif

#define is_alpha(c) (((c)>64 && (c)<91) || ((c)>96 && (c)<123))

#if defined(_WIN32) || defined(WIN32)
#define VISIBLE __declspec(dllexport)
#else
#define VISIBLE __attribute__((__visibility__("default")))
#endif

#define UNUSED __attribute__((__unused__))

int
VISIBLE
plugin_is_GPL_compatible = 1;

constexpr size_t k_header_statement_max_size = 20000;

struct seh_handler
{
    bool is_except;
    unsigned int line;
};

struct seh_function
{
    bool unwind;
    bool except;
    tree asm_header_text;
    tree asm_header;
    size_t count;
    std::vector<seh_handler> handlers;

    seh_function(struct function* fun)
        : unwind(false)
        , except(false)
        , count(0)
    {
        /* Reserve space for our header statement */
#if 0 // FIXME: crashes on older GCC
        asm_header_text = build_string(k_header_statement_max_size, "");
#else
        char buf[k_header_statement_max_size];
        memset(buf, 0, sizeof(buf));
        asm_header_text = build_string(sizeof(buf), buf);
#endif
        asm_header = build_stmt(fun->function_start_locus, ASM_EXPR, asm_header_text, NULL_TREE, NULL_TREE, NULL_TREE, NULL_TREE);
        ASM_VOLATILE_P(asm_header) = 1;
        add_stmt(asm_header);
    }
};

static std::unordered_map<struct function*, struct seh_function*> func_seh_map = {};

/* Functions with SEH scopes, kept until code generation (see seh_check_pass) */
static std::unordered_map<tree, location_t> seh_decls = {};

static
struct seh_function*
get_seh_function()
{
    auto search = func_seh_map.find(cfun);
    if (search != func_seh_map.end())
        return search->second;

    auto seh_fun = new seh_function(cfun);
    func_seh_map.insert({cfun, seh_fun});

    return seh_fun;
}

/*
 * SEH handlerdata is emitted per source function by this plugin.
 * If such a function gets inlined, GCC concatenates multiple handlerdata
 * blocks in the caller's xdata. __C_specific_handler expects one block.
 * Prevent inlining/cloning at the producer side to keep one canonical block.
 */
static
void
mark_seh_function_noinline(void)
{
    tree fndecl = current_function_decl;

    if (fndecl == NULL_TREE)
        return;

    DECL_UNINLINABLE(fndecl) = 1;
    DECL_DECLARED_INLINE_P(fndecl) = 0;
}

static
void
handle_seh_pragma(cpp_reader* UNUSED parser)
{
    tree x, arg, line;
    std::stringstream label_decl;
    bool is_except;

    if (!cfun)
    {
        error("%<#pragma REACTOS seh%> is not allowed outside functions");
        return;
    }

    if ((pragma_lex(&x) != CPP_OPEN_PAREN) ||
        (pragma_lex(&arg) != CPP_NAME) || // except or finally
        (pragma_lex(&x) != CPP_COMMA) ||
        (pragma_lex(&line) != CPP_NUMBER) || // Line number
        (pragma_lex(&x) != CPP_CLOSE_PAREN) ||
        (pragma_lex(&x) != CPP_EOF)
        )
    {
        error("%<#pragma REACTOS seh%> needs two parameters");
        return;
    }

    trace(stderr, "Pragma: %s, %u\n", IDENTIFIER_POINTER(arg), TREE_INT_CST_LOW(line));

    const char* op = IDENTIFIER_POINTER(arg);

    seh_function* seh_fun = get_seh_function();
    if (strcmp(op, "__seh$$except") == 0)
    {
        is_except = true;
        seh_fun->except = true;
    }
    else if (strcmp(op, "__seh$$finally") == 0)
    {
        is_except = false;
        seh_fun->unwind = true;
    }
    else
    {
        error("Wrong argument for %<#pragma REACTOS seh%>. Expected \"except\" or \"finally\"");
        return;
    }
    seh_fun->count++;

    seh_fun->handlers.push_back({is_except, (unsigned int)TREE_INT_CST_LOW(line)});

    /* Make sure we use a frame pointer. REACTOS' PSEH depends on this */
    cfun->machine->accesses_prev_frame = 1;

    /* Keep handlerdata generation canonical: one SEH block per function. */
    mark_seh_function_noinline();
}

static
void
finish_seh_function(void* event_data, void* UNUSED user_data)
{
    tree fndef = (tree)event_data;
    struct function* fun = DECL_STRUCT_FUNCTION(fndef);

    auto search = func_seh_map.find(fun);
    if (search == func_seh_map.end())
        return;

    /* Get our SEH details and remove us from the map */
    seh_function* seh_fun = search->second;
    func_seh_map.erase(search);

    /*
     * Whether the function also needs C++ exception handling is only known
     * after EH lowering (and inlining), so seh_check_pass checks it.
     */
    seh_decls.insert({fndef, DECL_SOURCE_LOCATION(fndef)});

    /* Update asm statement */
    std::stringstream asm_str;
    asm_str << ".seh_handler __C_specific_handler";
    if (seh_fun->unwind)
        asm_str << ", @unwind";
    if (seh_fun->except)
        asm_str << ", @except";
    asm_str << "\n";
    asm_str << "\t.seh_handlerdata\n";
    asm_str << "\t.long " << seh_fun->count << "\n";

    for (auto& handler : seh_fun->handlers)
    {
        /*
         * The scope is [__seh2$$begin_try__, __seh2$$end_try__): the try
         * body only. __C_specific_handler treats an unwind whose target
         * lies inside a scope as ending in that scope, so a scope that also
         * covered the code after the try body (e.g. up to the filter, which
         * GCC places after the function body) would keep the __finally of
         * an inner scope from running when an outer __except of the same
         * function catches. -fno-reorder-blocks (forced in plugin_init)
         * keeps the try body between the two labels.
         */
        asm_str << "\n\t.rva " << "__seh2$$begin_try__" << handler.line; /* Begin of tried code */
        asm_str << "\n\t.rva " << "__seh2$$end_try__" << handler.line; /* End of tried code */
        asm_str << "\n\t.rva " << "__seh2$$filter__" << handler.line; /* Filter function */
        if (handler.is_except)
            asm_str << "\n\t.rva " << "__seh2$$begin_except__" << handler.line; /* Called on except */
        else
            asm_str << "\n\t.long 0"; /* No unwind handler */
    }
    asm_str << "\n\t.seh_code\n";

    strncpy(const_cast<char*>(TREE_STRING_POINTER(seh_fun->asm_header_text)),
            asm_str.str().c_str(),
            TREE_STRING_LENGTH(seh_fun->asm_header_text));

    trace(stderr, "ASM: %s\n", asm_str.str().c_str());

    delete seh_fun;
}

/*
 * A function gets one SEH handler: __C_specific_handler for its scopes, or
 * GCC's personality for its C++ cleanups and catches, not both. Like MSVC
 * (C2712, C2713), refuse functions that need both; this runs after inlining
 * and EH lowering, so it also sees cleanups inlined from callees.
 */
static const pass_data seh_check_pass_data =
{
    RTL_PASS, "seh_check", OPTGROUP_NONE, TV_NONE, 0, 0, 0, 0, 0
};

struct seh_check_pass : rtl_opt_pass
{
    seh_check_pass(gcc::context* ctxt)
        : rtl_opt_pass(seh_check_pass_data, ctxt)
    {
    }

    unsigned int execute(function* fun) final override
    {
        auto search = seh_decls.find(fun->decl);
        if (search == seh_decls.end())
            return 0;
        location_t loc = search->second;
        seh_decls.erase(search);
        if (crtl->uses_eh_lsda)
            error_at(loc, "%qD uses %<__try%> and also needs C++ exception handling "
                     "(objects with destructors, try/catch, or functions inlined with them); "
                     "move the %<__try%> to a separate function", fun->decl);
        return 0;
    }
};

static
void
register_seh_pragmas(void* UNUSED event_data, void* UNUSED user_data)
{
    c_register_pragma("REACTOS", "seh", handle_seh_pragma);
}

/* Return 0 on success or error code on failure */
extern "C"
VISIBLE
int plugin_init(struct plugin_name_args   *info,  /* Argument infor */
                struct plugin_gcc_version *version)   /* Version of GCC */
{
    if (!plugin_default_version_check (version, &gcc_version))
    {
        std::cerr << "This GCC plugin is for version " << GCCPLUGIN_VERSION_MAJOR << "." << GCCPLUGIN_VERSION_MINOR << "\n";
        return 1;
    }

    /*
     * The scope table describes each try body as one address range, so its
     * basic blocks must stay in source order and in one section.
     */
    flag_reorder_blocks = 0;
    flag_reorder_blocks_and_partition = 0;

    struct register_pass_info check_pass;
    check_pass.pass = new seh_check_pass(g);
    check_pass.reference_pass_name = "final";
    check_pass.ref_pass_instance_number = 1;
    check_pass.pos_op = PASS_POS_INSERT_BEFORE;
    register_callback(info->base_name, PLUGIN_PASS_MANAGER_SETUP, NULL, &check_pass);

    register_callback(info->base_name, PLUGIN_PRAGMAS, register_seh_pragmas, NULL);
    register_callback(info->base_name, PLUGIN_FINISH_PARSE_FUNCTION, finish_seh_function, NULL);

    return 0;
}
