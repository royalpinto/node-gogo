//2011-11 Proyectos Equis Ka, s.l., jorge@jorgechamorro.com
//threads_a_gogo.cc

#include <v8.h>
#include <uv.h>
#include <node.h>
#include <node_version.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string>
#include <nan.h>

#define TAGG_USE_LIBUV
#if (NODE_MAJOR_VERSION == 0) && (NODE_MINOR_VERSION <= 5)
#undef TAGG_USE_LIBUV
#endif

#include "queues_a_gogo.cc"

//using namespace node;
using namespace v8;

static Nan::Persistent<String> id_symbol;
static Nan::Persistent<ObjectTemplate> threadTemplate;
static bool useLocker;

static typeQueue* freeJobsQueue= NULL;
static typeQueue* freeThreadsQueue= NULL;

#define kThreadMagicCookie 0x99c0ffee
typedef struct {
#ifdef TAGG_USE_LIBUV
  uv_async_t async_watcher; //MUST be the first one
#else
  ev_async async_watcher; //MUST be the first one
#endif
  
  int id;//TODO: Should be long
  pthread_t thread;
  volatile int sigkill;
  
  typeQueue inQueue;  //Jobs to run
  typeQueue outQueue; //Jobs done
  
  volatile int IDLE;
  pthread_cond_t IDLE_cv;
  pthread_mutex_t IDLE_mutex;
  
  Isolate* isolate;
  Persistent<Context> context;
  Persistent<Object> JSObject;
  Persistent<Object> threadJSObject;
  Persistent<Object> dispatchEvents;
  
  unsigned long threadMagicCookie;
} typeThread;

enum jobTypes {
  kJobTypeEval,
  kJobTypeEvent
};

typedef struct {
  int jobType;
  Persistent<Object> cb;
  union {
    struct {
      int length;
      String::Utf8Value* eventName;
      String::Utf8Value** argumentos;
    } typeEvent;
    struct {
      int error;
      int tiene_callBack;
      int useStringObject;
      String::Utf8Value* resultado;
      union {
        char* scriptText_CharPtr;
        String::Utf8Value* scriptText_StringObject;
      };
    } typeEval;
  };
} typeJob;

/*

cd deps/minifier/src
gcc minify.c -o minify
cat ../../../src/events.js | ./minify kEvents_js > ../../../src/kEvents_js
cat ../../../src/load.js | ./minify kLoad_js > ../../../src/kLoad_js
cat ../../../src/createPool.js | ./minify kCreatePool_js > ../../../src/kCreatePool_js
cat ../../../src/thread_nextTick.js | ./minify kThread_nextTick_js > ../../../src/kThread_nextTick_js

*/

#include "events.js.c"
//#include "load.js.c"
#include "createPool.js.c"
#include "thread_nextTick.js.c"
//#include "JASON.js.c"

//node-waf configure uninstall distclean configure build install








static typeQueueItem* nuJobQueueItem (void) {
  typeQueueItem* qitem= queue_pull(freeJobsQueue);
  if (!qitem) {
    qitem= nuItem(kItemTypePointer, calloc(1, sizeof(typeJob)));
  }
  return qitem;
}






static typeThread* isAThread (Handle<Object> receiver) {
  typeThread* thread;
  
  if (receiver->IsObject()) {
    if (receiver->InternalFieldCount() == 1) {
      thread= (typeThread*) Nan::GetInternalFieldPointer(receiver, 0);
      if (thread && (thread->threadMagicCookie == kThreadMagicCookie)) {
        return thread;
      }
    }
  }
  
  return NULL;
}






static void pushToInQueue (typeQueueItem* qitem, typeThread* thread) {
  pthread_mutex_lock(&thread->IDLE_mutex);
  queue_push(qitem, &thread->inQueue);
  if (thread->IDLE) {
    pthread_cond_signal(&thread->IDLE_cv);
  }
  pthread_mutex_unlock(&thread->IDLE_mutex);
}






NAN_METHOD(Puts) {
  //fprintf(stdout, "*** Puts BEGIN\n");
  
  int i= 0;
  while (i < info.Length()) {
    String::Utf8Value c_str(info[i]);
    fputs(*c_str, stdout);
    i++;
  }
  fflush(stdout);

  //fprintf(stdout, "*** Puts END\n");
  info.GetReturnValue().Set(Nan::Undefined());
}





static void eventLoop (typeThread* thread);

class ArrayBufferAllocator : public v8::ArrayBuffer::Allocator {
public:
 virtual void* Allocate(size_t length) {
  void* data = AllocateUninitialized(length);
  return data == NULL ? data : memset(data, 0, length);
 }
 virtual void* AllocateUninitialized(size_t length) { return malloc(length); }
 virtual void Free(void* data, size_t) { free(data); }
};


// A background thread
static void* aThread (void* arg) {
  
  int dummy;
  pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &dummy);
  pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &dummy);

  typeThread* thread= (typeThread*) arg;

// Create a new Isolate and make it the current one.
#if NODE_MODULE_VERSION >= NODE_4_0_MODULE_VERSION
  ArrayBufferAllocator allocator;
  Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = &allocator;
  thread->isolate= Isolate::New(create_params);
#else
  thread->isolate= Isolate::New();
#endif

  Nan::SetIsolateData(thread->isolate, thread);

  if (useLocker) {
    //printf("**** USING LOCKER: YES\n");
    v8::Locker myLocker(thread->isolate);
    //v8::Isolate::Scope isolate_scope(thread->isolate);
    eventLoop(thread);
  }
  else {
    //printf("**** USING LOCKER: NO\n");
    //v8::Isolate::Scope isolate_scope(thread->isolate);
    eventLoop(thread);
  }
  thread->isolate->Exit();
  thread->isolate->Dispose();
  
  // wake up callback
#ifdef TAGG_USE_LIBUV
  uv_async_send(&thread->async_watcher);
#else
  ev_async_send(EV_DEFAULT_UC_ &thread->async_watcher);
#endif
  
  return NULL;
}





NAN_METHOD(threadEmit);

static void eventLoop (typeThread* thread) {
  thread->isolate->Enter();
  Nan::HandleScope scope;
  Local<Context> context = Context::New(thread->isolate);
  thread->context.Reset(thread->isolate, context);
  context->Enter();

  {
    Nan::HandleScope scope1;

    Local<Object> global= context->Global();
    global->Set(Nan::New("puts").ToLocalChecked(), Nan::New<FunctionTemplate>(Puts)->GetFunction());
    Local<Object> threadObject= Nan::New<Object>();
    global->Set(Nan::New("thread").ToLocalChecked(), threadObject);

    threadObject->Set(Nan::New("id").ToLocalChecked(), Nan::New<Number>(thread->id));
    threadObject->Set(Nan::New("emit").ToLocalChecked(), Nan::New<FunctionTemplate>(threadEmit)->GetFunction());
    Local<Object> dispatchEvents= Script::Compile(Nan::New(kEvents_js).ToLocalChecked())->Run()->ToObject()->CallAsFunction(threadObject, 0, NULL)->ToObject();
    Local<Object> dispatchNextTicks= Script::Compile(Nan::New(kThread_nextTick_js).ToLocalChecked())->Run()->ToObject();
    Local<Array> _ntq= Local<Array>::Cast(threadObject->Get(Nan::New("_ntq").ToLocalChecked()));

    double nextTickQueueLength= 0;
    long int ctr= 0;
    
    //SetFatalErrorHandler(FatalErrorCB);
    
    while (!thread->sigkill) {
      typeJob* job;
      typeQueueItem* qitem;
      
      {
        Nan::HandleScope scope2;
        TryCatch onError;
        String::Utf8Value* str;
        Local<String> source;
        Local<Script> script;
        Local<Value> resultado;
        
        
        while ((qitem= queue_pull(&thread->inQueue))) {
          
          job= (typeJob*) qitem->asPtr;
          
          if ((++ctr) > 2e3) {
            ctr= 0;
            //TODO: V8::IdleNotification();
          }
          
          if (job->jobType == kJobTypeEval) {
            //Ejecutar un texto
            
            if (job->typeEval.useStringObject) {
              str= job->typeEval.scriptText_StringObject;
              source= Nan::New(**str, (*str).length()).ToLocalChecked();
              delete str;
            }
            else {
              source= Nan::New(job->typeEval.scriptText_CharPtr).ToLocalChecked();
              free(job->typeEval.scriptText_CharPtr);
            }
            
            
            script= Script::Compile(source);
            if (!onError.HasCaught()) resultado= script->Run();

            if (job->typeEval.tiene_callBack) {
              job->typeEval.error= onError.HasCaught() ? 1 : 0;
              job->typeEval.resultado= new String::Utf8Value(job->typeEval.error ? onError.Exception() : resultado);
              queue_push(qitem, &thread->outQueue);
              // wake up callback
#ifdef TAGG_USE_LIBUV
              uv_async_send(&thread->async_watcher);
#else
              ev_async_send(EV_DEFAULT_UC_ &thread->async_watcher);
#endif
            }
            else {
              queue_push(qitem, freeJobsQueue);
            }

            if (onError.HasCaught()) onError.Reset();
          }
          else if (job->jobType == kJobTypeEvent) {
            //Emitir evento.
            
            Local<Value> args[2];
            str= job->typeEvent.eventName;
            args[0]= Nan::New(**str, (*str).length()).ToLocalChecked();
            delete str;
            
            Local<Array> array= Nan::New<Array>(job->typeEvent.length);
            args[1]= array;
            
            int i= 0;
            while (i < job->typeEvent.length) {
              str= job->typeEvent.argumentos[i];
              array->Set(i, Nan::New(**str, (*str).length()).ToLocalChecked());
              delete str;
              i++;
            }
            
            free(job->typeEvent.argumentos);
            queue_push(qitem, freeJobsQueue);
            dispatchEvents->CallAsFunction(global, 2, args);
          }
        }
        
        if (_ntq->Length()) {
          
          if ((++ctr) > 2e3) {
            ctr= 0;
            // TODO: V8::IdleNotification();
          }
          
          resultado= dispatchNextTicks->CallAsFunction(global, 0, NULL);
          if (onError.HasCaught()) {
            nextTickQueueLength= 1;
            onError.Reset();
          }
          else {
            nextTickQueueLength= resultado->NumberValue();
          }
        }
      }
      
      if (nextTickQueueLength || thread->inQueue.length) continue;
      if (thread->sigkill) break;
      
      pthread_mutex_lock(&thread->IDLE_mutex);
      if (!thread->inQueue.length) {
        thread->IDLE= 1;
        pthread_cond_wait(&thread->IDLE_cv, &thread->IDLE_mutex);
        thread->IDLE= 0;
      }
      pthread_mutex_unlock(&thread->IDLE_mutex);
    }
  }
  
  thread->context.Reset();
}






static void destroyaThread (typeThread* thread) {
  
  Nan::HandleScope scope;
  thread->sigkill= 0;
  //TODO: hay que vaciar las colas y destruir los trabajos antes de ponerlas a NULL
  thread->inQueue.first= thread->inQueue.last= NULL;
  thread->outQueue.first= thread->outQueue.last= NULL;
  
  Nan::SetInternalFieldPointer(Nan::New(thread->JSObject), 0, NULL);
  thread->JSObject.Reset();
#ifdef TAGG_USE_LIBUV
  uv_close((uv_handle_t*) &thread->async_watcher, NULL);
  //uv_unref(&thread->async_watcher);
#else
  ev_async_stop(EV_DEFAULT_UC_ &thread->async_watcher);
  ev_unref(EV_DEFAULT_UC);
#endif
  
  if (freeThreadsQueue) {
    queue_push(nuItem(kItemTypePointer, thread), freeThreadsQueue);
  }
  else {
    free(thread);
  }
}






// C callback that runs in the main nodejs thread. This is the one responsible for
// calling the thread's JS callback.
static void Callback (
#ifdef TAGG_USE_LIBUV
  uv_async_t *watcher
#else
  EV_P_ ev_async *watcher
#endif
) {
  typeThread* thread= (typeThread*) watcher;
  
  if (thread->sigkill) {
    destroyaThread(thread);
    return;
  }
  
  Nan::HandleScope scope;
  typeJob* job;
  Local<Value> argv[2];
  Local<Value> null = Nan::Null();
  typeQueueItem* qitem;
  String::Utf8Value* str;
  
  Nan::TryCatch onError;
  while ((qitem= queue_pull(&thread->outQueue))) {
    job= (typeJob*) qitem->asPtr;

    if (job->jobType == kJobTypeEval) {

      if (job->typeEval.tiene_callBack) {
        str= job->typeEval.resultado;

        if (job->typeEval.error) {
          argv[0]= Exception::Error(Nan::New(**str, (*str).length()).ToLocalChecked());
          argv[1]= null;
        } else {
          argv[0]= null;
          argv[1]= Nan::New(**str, (*str).length()).ToLocalChecked();
        }
        Nan::New(job->cb)->CallAsFunction(Nan::New(thread->JSObject), 2, argv);
        job->cb.Reset();
        job->typeEval.tiene_callBack= 0;

        delete str;
        job->typeEval.resultado= NULL;
      }

      queue_push(qitem, freeJobsQueue);
      
      if (onError.HasCaught()) {
        if (thread->outQueue.first) {
#ifdef TAGG_USE_LIBUV
          uv_async_send(&thread->async_watcher); // wake up callback again
#else
          ev_async_send(EV_DEFAULT_UC_ &thread->async_watcher); // wake up callback again
#endif
        }
        Nan::FatalException(onError);
        return;
      }
    }
    else if (job->jobType == kJobTypeEvent) {
      
      //fprintf(stdout, "*** Callback\n");
      
      Local<Value> args[2];
      
      str= job->typeEvent.eventName;
      args[0]= Nan::New(**str, (*str).length()).ToLocalChecked();
      delete str;
      
      Local<Array> array= Nan::New<Array>(job->typeEvent.length);
      args[1]= array;
      
      int i= 0;
      while (i < job->typeEvent.length) {
        str= job->typeEvent.argumentos[i];
        array->Set(i, Nan::New(**str, (*str).length()).ToLocalChecked());
        delete str;
        i++;
      }
      
      free(job->typeEvent.argumentos);
      queue_push(qitem, freeJobsQueue);
      Nan::New(thread->dispatchEvents)->CallAsFunction(Nan::New(thread->JSObject), 2, args);
    }
  }
}






// unconditionally destroys a thread by brute force.
NAN_METHOD(Destroy) {
  //TODO: Hay que comprobar que this en un objeto y que tiene hiddenRefTotypeThread_symbol y que no es nil
  //TODO: Aquí habría que usar static void TerminateExecution(int thread_id);
  //TODO: static void v8::V8::TerminateExecution  ( Isolate *   isolate= NULL   )   [static]
  
  typeThread* thread= isAThread(info.This());
  if (!thread) {
    return Nan::ThrowTypeError(Nan::New("thread.destroy(): the receiver must be a thread object").ToLocalChecked());
  }
  
  if (!thread->sigkill) {
    //pthread_cancel(thread->thread);
    thread->sigkill= 1;
    pthread_mutex_lock(&thread->IDLE_mutex);
    if (thread->IDLE) {
      pthread_cond_signal(&thread->IDLE_cv);
    }
    pthread_mutex_unlock(&thread->IDLE_mutex);
  }
  
  info.GetReturnValue().Set(Nan::Undefined());
}






// Eval: Pushes a job into the thread's ->inQueue.
  
NAN_METHOD(Eval) {
  if (!info.Length()) {
    return Nan::ThrowTypeError(Nan::New("thread.eval(program [,callback]): missing arguments").ToLocalChecked());
  }
  
  typeThread* thread= isAThread(info.This());
  if (!thread) {
    return Nan::ThrowTypeError(Nan::New("thread.eval(): the receiver must be a thread object").ToLocalChecked());
  }

  typeQueueItem* qitem= nuJobQueueItem();
  typeJob* job= (typeJob*) qitem->asPtr;
  
  job->typeEval.tiene_callBack= ((info.Length() > 1) && (info[1]->IsFunction()));
  if (job->typeEval.tiene_callBack) {
    job->cb.Reset(info.GetIsolate(), info[1]->ToObject());
  }
  job->typeEval.scriptText_StringObject= new String::Utf8Value(info[0]);
  job->typeEval.useStringObject= 1;
  job->jobType= kJobTypeEval;
  
  pushToInQueue(qitem, thread);
  info.GetReturnValue().Set(info.This());
}





static char* readFile (Handle<String> path) {
  v8::String::Utf8Value c_str(path);
  FILE* fp= fopen(*c_str, "rb");
  if (!fp) {
    fprintf(stderr, "Error opening the file %s\n", *c_str);
    //@bruno: Shouldn't we throw, here ?
    return NULL;
  }
  fseek(fp, 0, SEEK_END);
  long len= ftell(fp);
  rewind(fp); //fseek(fp, 0, SEEK_SET);
  char *buf= (char*) calloc(len + 1, sizeof(char)); // +1 to get null terminated string
  fread(buf, len, 1, fp);
  fclose(fp);
  /*
  printf("SOURCE:\n%s\n", buf);
  fflush(stdout);
  */
  return buf;
}






// Load: Loads from file and passes to Eval
NAN_METHOD(Load) {

  if (!info.Length()) {
    return Nan::ThrowTypeError(Nan::New("thread.load(filename [,callback]): missing arguments").ToLocalChecked());
  }

  typeThread* thread= isAThread(info.This());
  if (!thread) {
    return Nan::ThrowTypeError(Nan::New("thread.load(): the receiver must be a thread object").ToLocalChecked());
  }
  
  char* source= readFile(info[0]->ToString());  //@Bruno: here we don't know if the file was not found or if it was an empty file
  if (!source) return info.GetReturnValue().Set(info.This()); //@Bruno: even if source is empty, we should call the callback ?

  typeQueueItem* qitem= nuJobQueueItem();
  typeJob* job= (typeJob*) qitem->asPtr;

  job->typeEval.tiene_callBack= ((info.Length() > 1) && (info[1]->IsFunction()));
  if (job->typeEval.tiene_callBack) {
    job->cb.Reset(info.GetIsolate(), info[1]->ToObject());
  }
  job->typeEval.scriptText_CharPtr= source;
  job->typeEval.useStringObject= 0;
  job->jobType= kJobTypeEval;

  pushToInQueue(qitem, thread);

  info.GetReturnValue().Set(info.This());
}






  
NAN_METHOD(processEmit) {
  //fprintf(stdout, "*** processEmit\n");
  
  
  if (!info.Length()) return info.GetReturnValue().Set(info.This());
  typeThread* thread= isAThread(info.This());
  if (!thread) {
    return Nan::ThrowTypeError(Nan::New("thread.emit(): the receiver must be a thread object").ToLocalChecked());
  }
  
  typeQueueItem* qitem= nuJobQueueItem();
  typeJob* job= (typeJob*) qitem->asPtr;
  
  job->jobType= kJobTypeEvent;
  job->typeEvent.length= info.Length()- 1;
  job->typeEvent.eventName= new String::Utf8Value(info[0]);
  job->typeEvent.argumentos= (v8::String::Utf8Value**) malloc(job->typeEvent.length* sizeof(void*));
  
  int i= 1;
  do {
    job->typeEvent.argumentos[i-1]= new String::Utf8Value(info[i]);
  } while (++i <= job->typeEvent.length);
  
  pushToInQueue(qitem, thread);
  
  info.GetReturnValue().Set(info.This());
}






  
NAN_METHOD(threadEmit) {
  //fprintf(stdout, "*** threadEmit\n");
  
  
  if (!info.Length()) info.GetReturnValue().Set(info.This());
  int i;
  
  typeThread* thread= (typeThread*) Nan::GetIsolateData<void *>(Isolate::GetCurrent());
  typeQueueItem* qitem= nuJobQueueItem();
  typeJob* job= (typeJob*) qitem->asPtr;
  
  job->jobType= kJobTypeEvent;
  job->typeEvent.length= info.Length()- 1;
  job->typeEvent.eventName= new String::Utf8Value(info[0]);
  job->typeEvent.argumentos= (v8::String::Utf8Value**) malloc(job->typeEvent.length* sizeof(void*));
  
  i= 1;
  do {
    job->typeEvent.argumentos[i-1]= new String::Utf8Value(info[i]);
  } while (++i <= job->typeEvent.length);
  
  queue_push(qitem, &thread->outQueue);
  
#ifdef TAGG_USE_LIBUV
  uv_async_send(&thread->async_watcher); // wake up callback
#else
  ev_async_send(EV_DEFAULT_UC_ &thread->async_watcher); // wake up callback
#endif
  
  //fprintf(stdout, "*** threadEmit END\n");
  
  info.GetReturnValue().Set(info.This());
}








// Creates and launches a new isolate in a new background thread.
NAN_METHOD(Create) {
    typeThread* thread;
    typeQueueItem* qitem= NULL;
    qitem= queue_pull(freeThreadsQueue);
    if (qitem) {
      thread= (typeThread*) qitem->asPtr;
      destroyItem(qitem);
    }
    else {
      thread= (typeThread*) calloc(1, sizeof(typeThread));
      thread->threadMagicCookie= kThreadMagicCookie;
    }
    
    static long int threadsCtr= 0;
    thread->id= threadsCtr++;
    
    
    Local<ObjectTemplate> localthreadTemplate = Nan::New(threadTemplate);
    thread->JSObject.Reset(info.GetIsolate(), localthreadTemplate->NewInstance());
    Local<Object> localJSObject = Nan::New(thread->JSObject);
    localJSObject->Set(Nan::New(id_symbol), Nan::New<Integer>(thread->id));
    Nan::SetInternalFieldPointer(localJSObject, 0, thread);
    Local<Value> dispatchEvents= Script::Compile(Nan::New(kEvents_js).ToLocalChecked())->Run()->ToObject()->CallAsFunction(localJSObject, 0, NULL);
    thread->dispatchEvents.Reset(info.GetIsolate(), dispatchEvents->ToObject());
#ifdef TAGG_USE_LIBUV
    uv_async_init(uv_default_loop(), &thread->async_watcher, Callback);
#else
    ev_async_init(&thread->async_watcher, Callback);
    ev_async_start(EV_DEFAULT_UC_ &thread->async_watcher);
    ev_ref(EV_DEFAULT_UC);
#endif
    
    pthread_cond_init(&thread->IDLE_cv, NULL);
    pthread_mutex_init(&thread->IDLE_mutex, NULL);
    pthread_mutex_init(&thread->inQueue.queueLock, NULL);
    pthread_mutex_init(&thread->outQueue.queueLock, NULL);
    if (pthread_create(&thread->thread, NULL, aThread, thread)) {
      //Ha habido un error no se ha arrancado este hilo
      destroyaThread(thread);
      Nan::ThrowTypeError(Nan::New("create(): error in pthread_create()").ToLocalChecked());
    }
    
    /*
    V8::AdjustAmountOfExternalAllocatedMemory(sizeof(typeThread));  //OJO V8 con V mayúscula.
    */
    
    info.GetReturnValue().Set(localJSObject);
}


void Init (Handle<Object> target) {
  
  initQueues();
  freeThreadsQueue= nuQueue(-3);
  freeJobsQueue= nuQueue(-4);
  
  Nan::HandleScope scope;
  
  useLocker= v8::Locker::IsActive();
  
  target->Set(Nan::New("create").ToLocalChecked(), Nan::New<FunctionTemplate>(Create)->GetFunction());
  target->Set(Nan::New("createPool").ToLocalChecked(), Script::Compile(Nan::New(kCreatePool_js).ToLocalChecked())->Run()->ToObject());
  //target->Set(String::NewSymbol("JASON"), Script::Compile(String::New(kJASON_js))->Run()->ToObject());
  

  id_symbol.Reset(Nan::New("id").ToLocalChecked());

  Local<ObjectTemplate> objectTemplate = ObjectTemplate::New();
  threadTemplate.Reset(objectTemplate);
  objectTemplate->SetInternalFieldCount(1);
  objectTemplate->Set(Nan::New(id_symbol), Nan::New<Integer>(0));
  objectTemplate->Set(Nan::New("eval").ToLocalChecked(), Nan::New<FunctionTemplate>(Eval)->GetFunction());
  objectTemplate->Set(Nan::New("load").ToLocalChecked(), Nan::New<FunctionTemplate>(Load)->GetFunction());
  objectTemplate->Set(Nan::New("emit").ToLocalChecked(), Nan::New<FunctionTemplate>(processEmit)->GetFunction());
  objectTemplate->Set(Nan::New("destroy").ToLocalChecked(), Nan::New<FunctionTemplate>(Destroy)->GetFunction());
}




NODE_MODULE(threads_a_gogo, Init)

/*
gcc -E -I /Users/jorge/JAVASCRIPT/binarios/include/node -o /o.c /Users/jorge/JAVASCRIPT/threads_a_gogo/src/threads_a_gogo.cc && mate /o.c
*/