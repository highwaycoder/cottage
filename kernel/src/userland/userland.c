// first-party headers
#include <userland/userland.h>
#include <mem/pagemap.h>
#include <mem/vmm.h>
#include <elf/elf.h>
#include <scheduler/scheduler.h>
#include <file/file.h>
#include <debug/debug.h>
#include <klog/klog.h>
#include <errors/errno.h>

// standard headers
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>

#define SHEBANG_MAX_INT_LEN 256
#define SHEBANG_MAX_ARG_LEN 256
#define MAX_RECURSION 4
// where interpreters are loaded
#define ELF_LD_LOAD_BASE 0x40000000

void parse_shebang(resource_t* resource, char** interpreter, char** arg);

process_t* userland_start_program(
    bool replace, 
    vfs_node_t* cwd,
    const char* path,
    size_t argc,
    const char* argv[],
    size_t envc,
    const char* envp[],
    const char* stdin,
    const char* stdout,
    const char* stderr,
    size_t recursion_depth)
{
    klog("user", "starting user program");
    vfs_node_t* prog_node = fs_get_node(cwd, path, true);
    if(prog_node == NULL)
    {
        // todo: error handling - we need to work out how to handle programs failing to load
        klog("user", "Could not load program: fs_get_node returned NULL");
        return NULL;
    }
    klog("user", "Got prog_node %p", prog_node);
    resource_t* prog = prog_node->resource;

    klog("user", "prog resource %p", prog);
	// create a new, blank pagemap
    pagemap_t* pagemap = malloc(sizeof(pagemap_t));
    klog("user", "pagemap %p", pagemap);
    * pagemap = new_pagemap();
    klog("user", "pagemap initialised successfully");

    char shebang[2] = {0};
    prog->read(prog, NULL, &shebang, 0, 2); 
    klog("user", "successfully read the first 2 bytes to check for shebang");
    if (strncmp(shebang, "#!", 2) == 0) // loading a script
    {
        klog("user", "determined that we are loading a script for execution by an interpreter");
        char interpreter[SHEBANG_MAX_INT_LEN] = {0};
        char arg[SHEBANG_MAX_ARG_LEN] = {0};
        size_t final_argc = 1; // count the interpreter to begin with
        parse_shebang(prog, (char**)&interpreter, (char**)&arg);
        char* final_argv[final_argc + (arg[0] != '\0' ? 1 : 0)];
        final_argv[0] = interpreter;
        if (arg[0] != '\0') {
            final_argc++;
            final_argv[1] = arg;
        }

        for(size_t i = final_argc; i < final_argc+argc; i++)
        {
            strcpy(final_argv[i], argv[i-final_argc]);
        }

		if(recursion_depth+1 > MAX_RECURSION)
        {
            set_errno(ELOOP);
            return NULL;
        }
        // we've taken a copy of this, so we can safely free the original
        // -- usually argv is free'd only when the program exits
        free(argv);
        return userland_start_program( replace,
                                cwd,
                                interpreter,
                                final_argc,
                                (const char**) final_argv,
                                envc,
                                envp,
                                stdin,
                                stdout,
                                stderr,
                                recursion_depth + 1
        );
    }
    else // loading an ELF file
    {
        klog("user", "determined that we are loading an ELF file");
        elf_info_t elf_info;
        klog("user", "running elf_load");
        bool success = elf_load(pagemap, prog, 0, &elf_info);
        if(!success)
        {
            klog("user", "elf_load fail");
            return NULL;
        }
        klog("user", "elf_load succeeded");
        void* entry_point = NULL;
        if(elf_info.ld_path == NULL)
        {
            klog("user", "ld_path was NULL");
            entry_point = (void*)elf_info.entry;
        }
        else
        {
            klog("user", "ld_path was not NULL");
            vfs_node_t* ld_node = fs_get_node(vfs_root, elf_info.ld_path, true);
            if(ld_node == NULL)
            {
                return NULL;
            }
            elf_info_t ld_info;
            elf_load(pagemap, ld_node->resource, ELF_LD_LOAD_BASE, &ld_info);
            entry_point = (void*)ld_info.entry;
        }
        klog("user", "entry point = %p", entry_point);
        // fork()
        if(replace)
        {
            klog("user", "replace = true");
            // use a sensible default size for new process names
            // maybe this should be pulled out into a macro or global const?
            const int default_name_size = 32;
            process_t* new_process = scheduler_new_process(NULL, pagemap);

            new_process->name = malloc(default_name_size);
            int namelen = snprintf(new_process->name, default_name_size, "%s[%lu]", path, new_process->pid);
            if(namelen > default_name_size)
            {
                new_process->name = realloc(new_process->name, namelen);
                snprintf(new_process->name, default_name_size, "%s[%lu]", path, new_process->pid);
            }

            // todo: refactor this to a macro/function? 
            vfs_node_t* stdin_node = fs_get_node(vfs_root, stdin, true);
            file_handle_t* stdin_handle = malloc(sizeof(file_handle_t));
            *stdin_handle = (file_handle_t){
                .resource = stdin_node->resource,
                .node = stdin_node,
                .refcount = 1
            };
            file_descriptor_t* stdin_fd = malloc(sizeof(file_descriptor_t));
            *stdin_fd = (file_descriptor_t){
                .handle = stdin_handle,
            };
            new_process->fds[0] = stdin_fd;

            vfs_node_t* stdout_node = fs_get_node(vfs_root, stdout, true);
            file_handle_t* stdout_handle = malloc(sizeof(file_handle_t));
            *stdout_handle = (file_handle_t){
                .resource = stdout_node->resource,
                .node = stdout_node,
                .refcount = 1
            };
            file_descriptor_t* stdout_fd = malloc(sizeof(file_descriptor_t));
            *stdout_fd = (file_descriptor_t){
                .handle = stdout_handle,
            };
            new_process->fds[1] = stdout_fd;

            vfs_node_t* stderr_node = fs_get_node(vfs_root, stderr, true);
            file_handle_t* stderr_handle = malloc(sizeof(file_handle_t));
            *stderr_handle = (file_handle_t){
                .resource = stderr_node->resource,
                .node = stderr_node,
                .refcount = 1
            };
            file_descriptor_t* stderr_fd = malloc(sizeof(file_descriptor_t));
            *stderr_fd = (file_descriptor_t){
                .handle = stderr_handle,
            };
            new_process->fds[2] = stderr_fd;

            new_user_thread(new_process, true, entry_point, NULL, 0, argc, argv, envc, envp, elf_info, true);

            return new_process;
        }
        else // execve()
        {
            klog("user", "replace = false (execve)");
            const int default_name_size = 32;
            thread_t* current_thread = get_current_thread();
            process_t* process = current_thread->process;
            pagemap_t* old_pagemap = process->pagemap;

            process->pagemap = pagemap;

            process->name = malloc(default_name_size);
            int namelen = snprintf(process->name, default_name_size, "%s[%lu]", path, process->pid);
            if(namelen > default_name_size)
            {
                process->name = realloc(process->name, namelen);
                snprintf(process->name, default_name_size, "%s[%lu]", path, process->pid);
            }

            switch_pagemap(&g_kernel_pagemap);
            current_thread->process = kernel_process;
            klog("user", "pagemap switched");

            delete_pagemap(old_pagemap);
            process->thread_stack_top = PROC_DEFAULT_THREAD_STACK_TOP;
            process->mmap_anon_non_fixed_base = PROC_DEFAULT_MMAP_ANON_NON_FIXED_BASE;

            klog("user", "old pagemap deleted");

            for(size_t i = 0; i < process->thread_count; i++)
            {
                // this is actually copied from VINIX, hehe... maybe I'll make a PR
                // and port my solution to V when I've finished?
                // todo: clean up process->threads[i];
                process->thread_count--;
            }

            klog("user", "creating new user thread");
            new_user_thread(process, true, entry_point, NULL, 0, argc, argv, envc, envp, elf_info, true);
            klog("user", "new user thread created");

			// for some reason, vinix frees these two arrays
            // but we don't know who has made them - we might be freeing
            // stack-allocated, or static-allocated memory!
            // we expect the caller to free them instead.
            // for(uint64_t i = 0; i < argc; i++)
            // {
            //     free(argv[i]);
            // }
            // free(argv);

            // for(uint64_t i = 0; i < envc; i++)
            // {
            //     free(envp[i]);
            // }
            // free(envp);
            scheduler_dequeue_and_die();
            return process;
        }
    }
}

// simple macro to make the code in parse_shebang a bit more readable
#define PSB_GETC() resource->read(resource, 0, &c, fseek_ptr++, 1)

void parse_shebang(resource_t* resource, char** interpreter, char** arg)
{
    size_t fseek_ptr = 0;
    char* interpreter_orig = *interpreter;
    char* arg_orig = *arg;
    char c = 0;
    PSB_GETC();
    // first character must be #
    assert( c == '#' );
    PSB_GETC();
    // second character must be !
    assert( c == '!' );

	// eat up whitespace chars
    do
    {
        PSB_GETC();
    }
    while( c == ' ');
    
    // grab the interpreter
    do
    {
        PSB_GETC();
        if(c == ' ') break;
        if(c == '\n') return;
        *(*interpreter++) = c;
    }
    while(*interpreter - interpreter_orig >= SHEBANG_MAX_INT_LEN);

	// grab the arg
    do
    {
        PSB_GETC();
        if(c == '\n') break;
        *(*arg++) = c;
    } while (*arg - arg_orig >= SHEBANG_MAX_ARG_LEN);
}
