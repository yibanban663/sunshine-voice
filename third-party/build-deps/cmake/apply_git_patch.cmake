# This macro applies patch to git repository if patch is applicable
# Arguments are path to git repository and path to the git patch

# APPLY_GIT_PATCH: args = `repo_path`, `patch_path`
macro(APPLY_GIT_PATCH repo_path patch_path)
    execute_process(COMMAND git apply -v --ignore-whitespace --check ${patch_path}
            WORKING_DIRECTORY ${repo_path}
            RESULT_VARIABLE SUCCESS
            COMMAND_ECHO STDOUT)

    if(${SUCCESS} EQUAL 0)
        message("Applying git patch ${patch_path} in ${repo_path} repository")
        execute_process(COMMAND git apply -v --ignore-whitespace ${patch_path}
                WORKING_DIRECTORY ${repo_path}
                RESULT_VARIABLE SUCCESS
                COMMAND_ECHO STDOUT)

        if(${SUCCESS} EQUAL 1)
            message(FATAL_ERROR "\n::error:: failed to apply the patch: ${patch_path}\n")
        endif()
    else()
        # Fallback for non-git directories (e.g., file(COPY) generated source)
        execute_process(COMMAND patch -p1 --dry-run -i ${patch_path}
                WORKING_DIRECTORY ${repo_path}
                RESULT_VARIABLE SUCCESS
                OUTPUT_VARIABLE PATCH_DRYRUN_OUTPUT
                ERROR_VARIABLE PATCH_DRYRUN_ERROR)
        if(${SUCCESS} EQUAL 0)
            message("Applying patch ${patch_path} in ${repo_path} (non-git)")
            execute_process(COMMAND patch -p1 -i ${patch_path}
                    WORKING_DIRECTORY ${repo_path}
                    RESULT_VARIABLE SUCCESS
                    COMMAND_ECHO STDOUT)
            if(NOT ${SUCCESS} EQUAL 0)
                message(FATAL_ERROR "\n::error:: failed to apply patch: ${patch_path}\n")
            endif()
        elseif(PATCH_DRYRUN_OUTPUT MATCHES "previously applied" OR PATCH_DRYRUN_ERROR MATCHES "previously applied")
            # Patch is already applied; nothing to do.
        else()
            message(WARNING "Patch ${patch_path} does not apply and was skipped\n"
                    "${PATCH_DRYRUN_OUTPUT}${PATCH_DRYRUN_ERROR}")
        endif()
    endif()
endmacro()
