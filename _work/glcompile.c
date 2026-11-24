/* glcompile <file.glsl> <vs|fs|cs>: compiles one shader as a separable program on the real driver, prints the time */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
typedef GLuint (APIENTRY *PCSP)(GLenum,GLsizei,const char* const*);
typedef void (APIENTRY *PGPI)(GLuint,GLenum,GLint*);
typedef void (APIENTRY *PGPL)(GLuint,GLsizei,GLsizei*,char*);
int main(int argc, char** argv){
  FILE* f=fopen(argv[1],"rb"); if(!f) return 2; fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
  char* s=malloc(n+1); fread(s,1,n,f); s[n]=0; fclose(f);
  HWND w=CreateWindowA("STATIC","x",WS_POPUP,0,0,8,8,0,0,0,0); HDC dc=GetDC(w);
  PIXELFORMATDESCRIPTOR pfd={sizeof pfd,1,PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER,PFD_TYPE_RGBA,32};
  SetPixelFormat(dc,ChoosePixelFormat(dc,&pfd),&pfd); HGLRC rc=wglCreateContext(dc); wglMakeCurrent(dc,rc);
  PCSP csp=(PCSP)wglGetProcAddress("glCreateShaderProgramv"); PGPI gpi=(PGPI)wglGetProcAddress("glGetProgramiv"); PGPL gpl=(PGPL)wglGetProcAddress("glGetProgramInfoLog");
  GLenum st= !strcmp(argv[2],"vs")? 0x8B31 : !strcmp(argv[2],"cs")? 0x91B9 : 0x8B30;
  LARGE_INTEGER fq,a,b; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&a);
  const char* src=s; GLuint p=csp(st,1,&src); GLint ok=0; gpi(p,0x8B82,&ok);
  glFinish(); QueryPerformanceCounter(&b);
  char log[2048]=""; gpl(p,sizeof log,NULL,log);
  printf("%s ok=%d %.1f ms %s\n",argv[1],ok,(b.QuadPart-a.QuadPart)*1000.0/fq.QuadPart, ok? "" : log);
  return 0; }
