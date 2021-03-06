#include "printf.h"
#include "keyboard.h"
#include "utils.h"
#include "console.h"
#include "../memorymap.h"
#include "memorypool.h"

#define MAX_CONSOLES 128

extern void* userAllocPages(uint64_t pageCount);
extern uint64_t getPTEntry(uint64_t virtual_address);
extern void memcpy64(char* source, char* dest, uint64_t size);
extern void memclear64(char* dest, uint64_t size);
extern void mutexLock(uint64_t*);
extern void mutexUnlock(uint64_t*);
extern void rwlockWriteLock(uint64_t*);
extern void rwlockWriteUnlock(uint64_t*);
extern void rwlockReadLock(uint64_t*);
extern void rwlockReadUnlock(uint64_t*);

void restoreTextConsole(struct ConsoleData* oldEntry);

volatile uint64_t frontLineSwitchLock;
volatile uint64_t frontLineConsoleIndex;
volatile uint64_t requestedConsoleIndex;
struct ConsoleData* consoles[MAX_CONSOLES];  
uint64_t consoleListLock;
uint64_t memoryPool;

//
// When a thread is dead, it wont attempt to access its console's screen
// since this is done my thread code using functions such as printf.
// so a dead console is guaranteed not to use its output buffer
// 
// On keyboard input, if the current frontline process is a dead
// process, then we risk writing in a buffer that does not exist anymore
// since the process memory might have been destroyed.
// For this reason, before killing a process, we must unregister
// its console.
//


// This function could be called on 2 CPUs at the same time but not for the same
// console since there are 1 console per thread.
// but eventually, when a process can havle multiple thread, they will share the same
// console so we will have to protect that.
void streamCharacters(char* str)
{
    struct ConsoleData* cd = *((struct ConsoleData**)CONSOLE_POINTER);
    if (cd == 0) return;
    while (*str!=0)
    {
if (((uint64_t)cd->flush_function)<100) __asm("int $3" : : "a"(cd->flush_function), "b"(cd));
        char c = *str;
        if (cd->streamPointer>=512)
        {
            cd->flush_function(cd);
        }
   
        if (c==0x03) // unconventional use of ascii char 0x03 to flush buffer
        {
            cd->flush_function(cd);
        }
        else
        {
            cd->streamBuffer[cd->streamPointer] = c;
            cd->streamPointer++;
            if (c=='\n') cd->flush_function();
        }
        str++;  
    } 
}

void scroll(char* buffer,uint32_t w, uint32_t h)
{
    memcpy64((char*)&buffer[w*2],(char*)&buffer[0],(w*(h-1)*2));
    memclear64((char*)&buffer[2*w*(h-1)],2*w);
}

void increaseBufferPointer(struct ConsoleData* cd, uint64_t count, char* buffer,uint32_t w, uint32_t h)
{
    cd->backBufferPointer+=count;
    if (cd->backBufferPointer >= (w*2*h))
    {
        scroll(buffer,w,h);
        cd->backBufferPointer-=(w*2);
    }
}

/////////////////////////////////////////////////////////////////////
// TODO: this could be written in ASM to increase performance
// Will handle:
//  cursror position
//  cursor move up,down,left,right
//  save/restore cursor
//  clear scren
//
/////////////////////////////////////////////////////////////////////
void handleANSI(struct ConsoleData *cd, char* buffer, char c)
{
    uint8_t i;
    uint32_t w,h;
    video_get_dimensions(cd->screen,&w,&h);
    cd->ansiData[cd->ansiIndex] = c;
    cd->ansiIndex++;

    if ((c>='a' && c <='z') || ((c>='A' && c <='Z')))
    {
        if (*((uint16_t*)cd->ansiData) == 0x5B1B)
        {
            if (c=='H' || c== 'f')
            {
                uint8_t num1 = 0;
                uint8_t num2 = 0;
                uint8_t* num = &num1;
                for (i=2;i<cd->ansiIndex-1;i++)
                {
                    char c = cd->ansiData[i];
                    if (c == ';')
                    {
                        num = &num2;
                    }
                    else if (c>='0'&&c<='9')
                    {
                        *num *= 10;
                        *num += (c-48);
                    }
                }
                if (num1<h && num2<w) cd->backBufferPointer = (num1*w*2)+(num2*2);
            }
            else if (c=='A' || c=='B' || c=='C' || c=='D')
            {
                uint8_t num = 0;
                for (i=2;i<cd->ansiIndex-1;i++)
                {
                    char c = cd->ansiData[i];
                    if (c>='0'&&c<='9')
                    {
                        num = 10;
                        num = (c-48);
                    }
                }

                if (c=='A')
                {
                    cd->backBufferPointer -= (num*w*2);
                    if (cd->backBufferPointer > (w*h*2)) cd->backBufferPointer=0;
                }
                else if (c=='B')
                {
                    cd->backBufferPointer += (num*w*2);
                    if (cd->backBufferPointer > (w*h*2)) cd->backBufferPointer=(w*h*2)-2;
                }
                else if (c=='C')
                {
                    cd->backBufferPointer -= (num*2);
                    if (cd->backBufferPointer > (w*h*2)) cd->backBufferPointer=0;
                }
                else if (c=='D')
                {
                    cd->backBufferPointer += (num*2);
                    if (cd->backBufferPointer > (w*h*2)) cd->backBufferPointer=(w*h*2)-2;
                }


            }
            else if (c=='J')
            {
                if (*((uint32_t*)cd->ansiData) == 0x4A325B1B)
                {
                    cd->backBufferPointer = 0;
                    memclear64(buffer,w*h*2);
                }
            }
            else if (c=='s')
            {
                cd->ansiSavedPosition = cd->backBufferPointer;
            }
            else if (c=='u')
            {
                cd->backBufferPointer = cd->ansiSavedPosition;
            }
            else if (c=='h')
            {
                if (*((uint32_t*)&cd->ansiData[2]) == 0x6835323F) cd->cursorOn = true;
            }
            else if (c=='l')
            {
                if (*((uint32_t*)&cd->ansiData[2]) == 0x6c35323F) cd->cursorOn = false;
            }
        }
        cd->ansiIndex = 0;
    }

    if (cd->ansiIndex == 8)
    {
        cd->ansiIndex = 0;
    }

}


void flushTextVideo()
{
    struct ConsoleData* cd = *((struct ConsoleData**)CONSOLE_POINTER);
    unsigned int i;
    uint64_t currentThreadID;
    char c;    
    char* outputBuffer;
    uint32_t w,h;
    
    video_get_dimensions(cd->screen,&w,&h);

    rwlockReadLock(&frontLineSwitchLock);
    outputBuffer = video_get_buffer(cd->screen); 

    for (i=0;i<cd->streamPointer;i++)
    {
        c = cd->streamBuffer[i];
        if (c == 0x1B || cd->ansiIndex!=0)
        {
            handleANSI(cd, outputBuffer, c);
        }
        else if (c==0x08)
        {
            cd->backBufferPointer -= 2; 
        }
        else if (c=='\r')
        {
            cd->backBufferPointer -= (cd->backBufferPointer % (w*2));
        }
        else if (c=='\n')
        {
            increaseBufferPointer(cd,(w*2),outputBuffer,w,h);
        }
        else if (c=='\t')
        {
            increaseBufferPointer(cd,8,outputBuffer,w,h);
        }
        else if (c=='\t')
        {
            if (cd->backBufferPointer >= 2)
            {
                cd->backBufferPointer -= 2; 
                outputBuffer[cd->backBufferPointer]=0;
            }
        }
        else
        {
            outputBuffer[cd->backBufferPointer] = c;
            increaseBufferPointer(cd,2, outputBuffer,w,h);
        }
    }
    cd->streamPointer = 0;
    video_update_cursor(cd->screen, cd->cursorOn, cd->backBufferPointer>>1);
    rwlockReadUnlock(&frontLineSwitchLock);
}


void initConsoles()
{
    int i;
    frontLineConsoleIndex = -1;
    requestedConsoleIndex = -1;
    frontLineSwitchLock = 0;
    consoleListLock = 0;
    memoryPool = create_memory_pool(sizeof(struct ConsoleData));

    for (i=0;i<MAX_CONSOLES;i++) consoles[i] = 0;
}

void storeCharacter(uint16_t c)
{
    // We switch console focus using F2-F12, but this only allows us to use 12 consoles.
    // TODO: should find another way to let user choose its console
    if (c>=KEY_F2 && c<=KEY_F12)
    {
        requestedConsoleIndex = c-KEY_F2;
        return;
    }

    if (frontLineConsoleIndex == -1) return;


    struct ConsoleData* cd = consoles[frontLineConsoleIndex];
    if (cd == 0) return;    // console has been removed
//TODO:  another CPU could have change frontlneConsoleIndex at this point. Does it matter?

    uint64_t n = (cd->kQueueIn+1)&0x0F;
    if (n==cd->kQueueOut) return;

    cd->keyboardBuffer[cd->kQueueIn] = c;
    cd->kQueueIn = n;
    
}

uint16_t pollChar()
{
    uint16_t ret;
    struct ConsoleData* cd = *((struct ConsoleData**)CONSOLE_POINTER);
    if (cd->kQueueOut == cd->kQueueIn) return 0;
    ret =  cd->keyboardBuffer[cd->kQueueOut];
    cd->kQueueOut = (cd->kQueueOut+1)&0x0F;
    return ret;
}

void destroy_text_console_handle(system_handle* handle)
{
    struct ConsoleData* h = (struct ConsoleData*)handle;
    restoreTextConsole(h->previousOwningProcess); 
    release_object(memoryPool,h);
}

Screen* getDirectVideo()
{
    struct ConsoleData* c = *((struct ConsoleData**)CONSOLE_POINTER);
//__asm__("int $3": : "a"(c->screen));
    return c->screen;
}

struct ConsoleData* createTextConsoleForProcess()
{
    struct ConsoleData** consoleDataPointer;
    consoleDataPointer = (struct ConsoleData**)CONSOLE_POINTER;

    //struct ConsoleData* consoleInfo = (struct ConsoleData*)malloc(sizeof(struct ConsoleData));
    struct ConsoleData* consoleInfo = (struct ConsoleData*)reserve_object(memoryPool);
    *consoleDataPointer = consoleInfo;

    memclear64(consoleInfo,sizeof(struct ConsoleData));
    //if (consoles[0] != 0) __asm("mov %0,%%rax; int $3" : : "r"(consoleInfo->backBuffer));
    consoleInfo->handle.destructor = &destroy_text_console_handle;
    consoleInfo->streamPointer = 0;
    consoleInfo->backBufferPointer = 0;
    consoleInfo->kQueueIn = 0;
    consoleInfo->kQueueOut = 0;
    consoleInfo->flush_function = &flushTextVideo;
    consoleInfo->lock = 0;
    consoleInfo->ansiIndex = 0;
    consoleInfo->ansiSavedPosition = 0;
    consoleInfo->cursorOn = true;
    consoleInfo->previousOwningProcess = 0;
    __asm("mov %%cr3,%0" : "=r"(consoleInfo->owningProcess));
    consoleInfo->owningProcess &= 0x00FFFFFFFFFFF000LL;

    return consoleInfo;

}

// This will give back the console to a process that got its
// console stolen by another process
void restoreTextConsole(struct ConsoleData* oldEntry)
{
    uint64_t i;
    uint64_t currentProcess;
    __asm("mov %%cr3,%0" : "=r"(currentProcess));
    currentProcess &= 0x00FFFFFFFFFFF000LL;

    mutexLock(&consoleListLock);
    for (i=0;i<MAX_CONSOLES;i++)
    {
        if (consoles[i] == 0) continue;
        if (consoles[i]->owningProcess == currentProcess)
        {
            if (oldEntry == 0)
            {
                // TODO: by setting this to zero, we prevent keyboard handler to
                // write ti keyboard buffer when the frontline process is still
                // this process but its console was removed. But there is a window
                // where the keyboard has checked for zero and writes into the buffer.
                // if we set this to zero during that window, and we continue to
                // set the process as dead, and a 3rd cpu runs kernelmain and
                // destroys the memory of that process, then they keyboard handler
                // will fault. Technically, this is impossible since the number
                // of instructions after this function, and the number of instructions
                // in the memory destruction of the process is greater than the keyboard
                // handler. So the keyboard will have exited by the time that the
                // buffer gets destroyed. But this is non-deterministic. It
                // would be a good thing to make 100% sure that this cannot happen.
                // CPU0                CPU1                CPU2
                // keyb_handler        ...                 ...
                // ...                 removeConsole       ...
                // ...                 set dead            ...
                // ...                 get scheduled out   ...
                // ...                 ...                 destroyMem
                // ERROR               ...
                // keyb_handler_end    ...
                //
                // but by the time CPU1 sets process as dead and schedules it out,
                // keyboard handler will have terminated

                consoles[i] = 0;
            }
            else
            {
            //TODO: delete this    memcpy64(consoles[i]->backBuffer,oldEntry->backBuffer,(2*80*25));
                oldEntry->backBufferPointer = consoles[i]->backBufferPointer;
                consoles[i] = oldEntry;
            }
            mutexUnlock(&consoleListLock);
            return;
        }
    }
    mutexUnlock(&consoleListLock);
}

// This will allow a process to takeover an existing console
uint64_t stealTextConsole(uint64_t processID)
{
    uint64_t i;
    struct ConsoleData* entry = createTextConsoleForProcess();
    processID &= 0x00FFFFFFFFFFF000LL;

    mutexLock(&consoleListLock);
    for (i=0;i<MAX_CONSOLES;i++)
    {
        if (consoles[i] == 0) continue;
        if (consoles[i]->owningProcess == processID)
        {
            struct ConsoleData* oldEntry = consoles[i];
            consoles[i] = entry;
//TODO:            memcpy64(oldEntry->backBuffer,consoles[i]->backBuffer,(2*80*25));
            consoles[i]->screen = oldEntry->screen;
            consoles[i]->backBufferPointer = oldEntry->backBufferPointer;
            consoles[i]->previousOwningProcess = oldEntry;
            mutexUnlock(&consoleListLock);
            return (uint64_t)oldEntry;
        }
    }
    mutexUnlock(&consoleListLock);
    return 0;
}

void createTextConsole()
{
    uint64_t i;

    struct ConsoleData* entry = createTextConsoleForProcess();
    entry->screen = video_create_screen();
    mutexLock(&consoleListLock);
    //TODO: must handle the case where no more consoles are available
    for (i=0;i<MAX_CONSOLES;i++)
    {
        if (consoles[i] == 0)
        {
            consoles[i] = entry;
            //__asm("mov %0,%%rax; int $3" : : "r"(consoles[i]));
            break;
        }
    }
    mutexUnlock(&consoleListLock);
}

void removeConsole()
{
    uint64_t i;
    //TODO: remove screen also
    struct ConsoleData* entry = *((struct ConsoleData**)CONSOLE_POINTER);
    for (i=0;i<MAX_CONSOLES;i++)
    {
        if (consoles[i] == entry)
        {
            system_handle* h = (system_handle*)consoles[i];
            h->destructor(h);
            *((struct ConsoleData**)CONSOLE_POINTER) = 0;
            return;
        }
    }
}

void switchFrontLineProcessByIndex(uint64_t index)
{
    unsigned int i;
    if (index >= MAX_CONSOLES) return;
    if (consoles[index] == 0) return;

    rwlockWriteLock(&frontLineSwitchLock);
    if (frontLineConsoleIndex != -1 && consoles[frontLineConsoleIndex]!=0)
    {
        video_change_active_screen(consoles[frontLineConsoleIndex]->screen,consoles[index]->screen);
    }
    else
    {
        video_change_active_screen(0,consoles[index]->screen);
    }
    video_update_cursor(consoles[index]->screen,consoles[index]->cursorOn,consoles[index]->backBufferPointer>>1);
    frontLineConsoleIndex = index;

    rwlockWriteUnlock(&frontLineSwitchLock);
}


void manageConsoles()
{
    uint64_t index = requestedConsoleIndex;

    if (index != frontLineConsoleIndex)
    {
        switchFrontLineProcessByIndex(index);
    }
}

