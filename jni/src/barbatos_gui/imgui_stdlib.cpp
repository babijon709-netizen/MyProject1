#include "misc/cpp/imgui_stdlib.h"
#include <vector>
#include <cstring>
namespace ImGui {
namespace { struct Data { std::string* s; }; static int Callback(ImGuiInputTextCallbackData* d) { auto* x=static_cast<Data*>(d->UserData); if(d->EventFlag==ImGuiInputTextFlags_CallbackResize){ x->s->resize(d->BufTextLen); d->Buf=x->s->data(); } return 0; } }
bool InputText(const char* label,std::string* s,ImGuiInputTextFlags f,ImGuiInputTextCallback cb,void* ud){ Data x{s}; if(s->capacity()<s->size()+1)s->reserve(s->size()+16); f|=ImGuiInputTextFlags_CallbackResize; bool r=InputText(label,s->data(),s->capacity()+1,f,cb?cb:Callback,cb?ud:&x); if(r)s->resize(strlen(s->c_str())); return r; }
bool InputTextMultiline(const char* label,std::string* s,const ImVec2& size,ImGuiInputTextFlags f,ImGuiInputTextCallback cb,void* ud){ Data x{s}; if(s->capacity()<s->size()+1)s->reserve(s->size()+16); f|=ImGuiInputTextFlags_CallbackResize; bool r=InputTextMultiline(label,s->data(),s->capacity()+1,size,f,cb?cb:Callback,cb?ud:&x); if(r)s->resize(strlen(s->c_str())); return r; }
}
