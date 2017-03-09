#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

#include "ccan/json/json.h"

#define panic()                                           \
  do {                                                    \
    DWORD __attribute__((unused)) error = GetLastError(); \
    DebugBreak();                                         \
  } while (0)

size_t available_stack() {
  MEMORY_BASIC_INFORMATION mbi;
  VirtualQuery(&mbi, &mbi, sizeof(mbi));
  VirtualQuery(mbi.AllocationBase, &mbi, sizeof(mbi));
  return mbi.RegionSize;  // actually this plus guard page
}

int main(int argc, char** argv) {
  PROCESS_INFORMATION process_info = {};

  STARTUPINFO startup_info = {.cb = sizeof(STARTUPINFO),
                              .dwFlags = STARTF_USESTDHANDLES};

  HANDLE child_in_read;
  HANDLE child_in_write;
  HANDLE child_out_read;
  HANDLE child_out_write;

  SECURITY_ATTRIBUTES sa = {.nLength = sizeof(SECURITY_ATTRIBUTES),
                            .bInheritHandle = TRUE};

  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);

  // todo: switch to named pipes and overlapped io
  // const char* child_in_name = "\\\\.\\pipe\fixme_in";
  // const char* child_out_name = "\\\\.\\pipe\fixme_out";

  CreatePipe(&child_in_read, &child_in_write, &sa, 0);
  CreatePipe(&child_out_read, &child_out_write, &sa, 0);
  SetHandleInformation(child_out_read, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(child_in_write, HANDLE_FLAG_INHERIT, 0);

  startup_info.hStdError = child_out_write;
  startup_info.hStdOutput = child_out_write;
  startup_info.hStdInput = child_in_read;

  while (true) {
    // DWORD waker = WaitForMultipleObjects(
    //    sizeof(wait_handles)/sizeof(wait_handles[0]),
    //    wait_handles,
    //    FALSE, //wait for any
    //    INFINITE); // consider timeout?

    Sleep(10);  // todo: switch to overlapped io and waitformultipleobjects

    // check stdin
    DWORD available;
    INPUT_RECORD r;

    if (!PeekConsoleInput(in, &r, 1, &available)) {
      if (!PeekNamedPipe(in, NULL, 0, NULL, &available, NULL)) {
        panic();  // exit gracefully
      }
    }

    if (available > 0) {
      uint32_t message_length;

      if (!ReadFile(in, &message_length, sizeof(message_length), NULL, NULL)) {
        panic();
      }

      JsonNode* message;

      if (message_length > available_stack() / 2) {
        panic();
      } else {
        char mem[message_length + 1];
        DWORD bytes_read;  // should not do this, messages could be fragmented?
        if (!ReadFile(in, mem, message_length, &bytes_read, NULL)) {
          panic();  // exit gracefully on EOF
        }
        mem[bytes_read] = '\0';

        message = json_decode((const char*)mem);
        if (!message) {
          panic();
        }
      }

      if (message->tag == JSON_STRING) {
        if (process_info.dwProcessId) {
          if (!WriteFile(child_in_write, message->string_,
                         strlen(message->string_), NULL, NULL)) {
            panic();
          }
        }
      } else if (message->tag == JSON_OBJECT) {
        JsonNode* operation = json_find_member(message, "operation");
        if (operation->tag != JSON_STRING) panic();

        if (strcmp(operation->string_, "run") == 0) {
          JsonNode* command = json_find_member(message, "command");
          JsonNode* args = json_find_member(message, "args");

          if (command && command->tag == JSON_STRING &&
              (!args || args->tag == JSON_STRING)) {
            if (!CreateProcess(command->string_,
                               args ? args->string_ : NULL,  // command line
                               NULL,  // process security attributes
                               NULL,  // primary thread security attributes
                               TRUE,  // handles are inherited
                               0,     // creation flags
                               NULL,  // use parent's environment
                               NULL,  // use parent's current directory
                               &startup_info,   // STARTUPINFO pointer
                               &process_info))  // receives PROCESS_INFORMATION
            {
              // fail gracefully?
              panic();
            }
          }
        } else if (strcmp(operation->string_, "kill") == 0) {
          if (process_info.hProcess) {
            // todo: send ctrl-c/wm_quit unless force
            if (!TerminateProcess(process_info.hProcess, 1)) {
              // fail gracefully? (e.g. process changed uid
            }
          } else {
            // nothing running
          }
        }
      } else {
        panic();
      }

      json_delete(message);
    } /*else {
        //other console events (mouse) are waking us. clear them.
        if (!FlushConsoleInputBuffer(in)) panic();
    }*/

    // try to read from the child process (if connected)

    if (!PeekNamedPipe(child_out_read, NULL, 0, NULL, &available, NULL)) {
      panic();
    }

    if (available > available_stack() / 8) {
      available = available_stack() / 8;
    }

    if (available > 0) {
      char buffer[available + 1];
      DWORD bytes_read;
      ReadFile(child_out_read, buffer, available, &bytes_read, NULL);
      buffer[bytes_read] = '\0';
      char* message = json_encode_string(buffer);
      uint32_t message_len = strlen(message);
      if (!WriteFile(out, &message_len, sizeof(message_len), NULL, NULL)) {
        panic();  // todo:quit gracefully when chrome closes pipe
      }
      if (!WriteFile(out, message, strlen(message), NULL, NULL)) {
        panic();
      }
      free(message);
    }

    if (process_info.hProcess) {
      // GetExitCodeProcess complains INVALID_HANDLE, but GetExitCodeThread
      // works unless the program is multithreaded (FIXME)
      DWORD exit_code;
      if (!GetExitCodeThread(process_info.hThread, &exit_code)) {
        exit_code = 0;  // :/
      }

      if (exit_code != STILL_ACTIVE
          // or process exited with status 259 (STILL_ACTIVE!?!?)
          || !WaitForSingleObject(process_info.hProcess, 0)) {
        // clean up
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        ZeroMemory(&process_info, sizeof(process_info));

        // inform the others
        JsonNode* o = json_mkobject();
        JsonNode* type = json_mkstring("exit");
        JsonNode* status = json_mknumber(exit_code);
        json_append_member(o, "type", type);
        json_append_member(o, "status", status);
        char* message = json_encode(o);
        uint32_t message_len = strlen(message);
        if (!WriteFile(out, &message_len, sizeof(message_len), NULL, NULL)) {
          panic();  // todo:quit gracefully when chrome closes pipe
        }
        if (!WriteFile(out, message, strlen(message), NULL, NULL)) {
          panic();
        }

        json_delete(o);
        free(message);
      }
    }
  }
}
