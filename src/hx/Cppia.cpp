#include <hxcpp.h>
#include <hx/Scriptable.h>
#include <hx/GC.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <map>

#include "Cppia.h"

//#define DBGLOG(...) { }
#define DBGLOG printf


namespace hx
{

// todo - TLS
static CppiaCtx *sCurrent = 0;

CppiaCtx::CppiaCtx()
{
   stack = new unsigned char[64*1024];
   pointer = &stack[0];
   push((hx::Object *)0);
   frame = pointer;
   sCurrent = this;
}

CppiaCtx::~CppiaCtx()
{
   delete [] stack;
}

CppiaCtx *CppiaCtx::getCurrent() { return sCurrent; }



class ScriptRegistered
{
public:
   std::string  name;
   hx::ScriptableClassFactory factory;
   hx::ScriptFunction  construct;
   ScriptNamedFunction *functions;
   ScriptRegistered *haxeSuper;
   int mDataOffset;

   ScriptRegistered(const std::string &inName, int inDataOffset, ScriptNamedFunction *inFunctions, hx::ScriptableClassFactory inFactory, ScriptFunction inConstruct)
   {
      name = inName;
      mDataOffset = inDataOffset;
      functions = inFunctions;
      factory = inFactory;
      construct = inConstruct;
      haxeSuper = 0;
   }

   void addVtableEntries( std::vector<std::string> &outVtable)
   {
      if (haxeSuper)
         haxeSuper->addVtableEntries(outVtable);

      if (functions)
         for(ScriptNamedFunction *func = functions; func->name; func++)
            outVtable.push_back( func->name );
   }

   ScriptFunction findFunction(const std::string &inName)
   {
      if (functions)
         for(ScriptNamedFunction *f=functions;f->name;f++)
            if (inName == f->name)
               return *f;
      if (haxeSuper)
         return haxeSuper->findFunction(inName);

      return ScriptFunction(0,0);
   }

};

class ScriptRegisteredIntferface
{
public:
   const hx::type_info *mType;
   ScriptableInterfaceFactory factory;

   ScriptRegisteredIntferface(hx::ScriptableInterfaceFactory inFactory,const hx::type_info *inType)
   {
      factory = inFactory;
      mType = inType;
   }
};


typedef std::map<std::string, ScriptRegistered *> ScriptRegisteredMap;
static ScriptRegisteredMap *sScriptRegistered = 0;
static ScriptRegistered *sObject = 0;

typedef std::map<std::string, ScriptRegisteredIntferface *> ScriptRegisteredInterfaceMap;
static ScriptRegisteredInterfaceMap *sScriptRegisteredInterface = 0;


// TODO - toString?
class Object_obj__scriptable : public Object
{
   typedef Object_obj__scriptable __ME;
   typedef Object super;

   void __construct() { }
   HX_DEFINE_SCRIPTABLE(HX_ARR_LIST0)
   HX_DEFINE_SCRIPTABLE_DYNAMIC;
};



void ScriptableRegisterClass( String inName, int inDataOffset, ScriptNamedFunction *inFunctions, hx::ScriptableClassFactory inFactory, hx::ScriptFunction inConstruct)
{

   printf("ScriptableRegisterClass %s\n", inName.__s);
   if (!sScriptRegistered)
      sScriptRegistered = new ScriptRegisteredMap();
   ScriptRegistered *registered = new ScriptRegistered(inName.__s,inDataOffset, inFunctions,inFactory, inConstruct);
   (*sScriptRegistered)[inName.__s] = registered;
   //printf("Registering %s -> %p\n",inName.__s,(*sScriptRegistered)[inName.__s]);
}


void ScriptableRegisterInterface( String inName, const hx::type_info *inType,
                                 hx::ScriptableInterfaceFactory inFactory )
{
   if (!sScriptRegisteredInterface)
      sScriptRegisteredInterface = new ScriptRegisteredInterfaceMap();
   ScriptRegisteredIntferface *registered = new ScriptRegisteredIntferface(inFactory,inType);
   (*sScriptRegisteredInterface)[inName.__s] = registered;
   //printf("Registering Interface %s -> %p\n",inName.__s,(*sScriptRegisteredInterface)[inName.__s]);
}


static int sTypeSize[] = { 0, 0, sizeof(hx::Object *), sizeof(String), sizeof(Float), sizeof(int) };

struct TypeData
{
   String           name;
   Class            haxeClass;
   CppiaClass       *cppiaClass;
   ExprType         expressionType;
   ScriptRegistered *haxeBase;
   bool             linked;
   ArrayType        arrayType;

   TypeData(String inData)
   {
      Array<String> parts = inData.split(HX_CSTRING("::"));
      if (parts[0].length==0)
         parts->shift();
      name = parts->join(HX_CSTRING("."));
      cppiaClass = 0;
      haxeClass = null();
      haxeBase = 0;
      linked = false;
      arrayType = arrNotArray;
   }
   void mark(hx::MarkContext *__inCtx)
   {
      HX_MARK_MEMBER(name);
      HX_MARK_MEMBER(haxeClass);
   }
   void visit(hx::VisitContext *__inCtx)
   {
      HX_VISIT_MEMBER(name);
      HX_VISIT_MEMBER(haxeClass);
   }

   void link(CppiaData &inData);
};

struct StackLayout;

struct CppiaData
{
   Array< String > strings;
   std::vector< TypeData * > types;
   std::vector< CppiaClass * > classes;
   std::vector< CppiaExpr * > markable;
   CppiaExpr   *main;

   StackLayout *layout;

   CppiaData()
   {
      main = 0;
      layout = 0;
      strings = Array_obj<String>::__new(0,0);
   }

   //CppiaClass *findClass(String inName);

   ~CppiaData();

   void link();

   void mark(hx::MarkContext *ctx);
   void visit(hx::VisitContext *ctx);

   const char *identStr(int inId) { return strings[inId].__s; }
   const char *typeStr(int inId) { return types[inId]->name.c_str(); }
};


enum VarLocation
{
   locObj,
   locThis,
   locStack,
};

struct CppiaStackVar
{
   int  nameId;
   int  id;
   bool capture;
   int  typeId;
   int  stackPos;
   int  fromStackPos;
   int  capturePos;

   ExprType expressionType;

   CppiaStackVar()
   {
      nameId = 0;
      id = 0;
      capture = false;
      typeId = 0;
      stackPos = 0;
      fromStackPos = 0;
      capturePos = 0;
      expressionType = etNull;
   }

   CppiaStackVar(CppiaStackVar *inVar,int &ioSize, int &ioCaptureSize)
   {
      nameId = inVar->nameId;
      id = inVar->id;
      capture = inVar->capture;
      typeId = inVar->typeId;
      expressionType = inVar->expressionType;

      fromStackPos = inVar->stackPos;
      stackPos = ioSize;
      capturePos = ioCaptureSize;
      ioSize += sTypeSize[expressionType];
      ioCaptureSize += sTypeSize[expressionType];
   }


   void fromStream(CppiaStream &stream)
   {
      nameId = stream.getInt();
      id = stream.getInt();
      capture = stream.getBool();
      typeId = stream.getInt();
   }

   void link(CppiaData &inData);
};




struct StackLayout
{
   std::map<int,CppiaStackVar *> varMap;
   std::vector<CppiaStackVar *>  captureVars;
   StackLayout *parent;
   int captureSize;
   int size;

   // 'this' pointer is in slot 0 and captureSize(0) ...
   StackLayout(StackLayout *inParent) :
      size( sizeof(void *) ), captureSize(sizeof(void *)), parent(inParent)
   {
   }

   CppiaStackVar *findVar(int inId)
   {
      if (varMap[inId])
         return varMap[inId];
      CppiaStackVar *var = parent ? parent->findVar(inId) : 0;
      if (!var)
         return 0;

      CppiaStackVar *closureVar = new CppiaStackVar(var,size,captureSize);
      varMap[inId] = closureVar;
      captureVars.push_back(closureVar);
      return closureVar;
   }

};

void CppiaStackVar::link(CppiaData &inData)
{
   expressionType = inData.types[typeId]->expressionType;
   inData.layout->varMap[id] = this;
   stackPos = inData.layout->size;
   inData.layout->size += sTypeSize[expressionType];
}



class CppiaObject : public hx::Object
{
public:
   CppiaData *data;
   CppiaObject(CppiaData *inData)
   {
      data = inData;
      GCSetFinalizer( this, onRelease );
   }
   static void onRelease(hx::Object *inObj)
   {
      delete ((CppiaObject *)inObj)->data;
   }
   void __Mark(hx::MarkContext *ctx) { data->mark(ctx); }
   void __Visit(hx::VisitContext *ctx) { data->visit(ctx); }
};

struct ArgInfo
{
   int  nameId;
   bool optional;
   int  typeId;
};


// --- CppiaCtx functions ----------------------------------------

int CppiaCtx::runInt(void *vtable)
{
   try
   {
      ((CppiaExpr *)vtable)->runVoid(this);
   }
   catch (CppiaExpr *retVal)
   {
      return retVal->runInt(this);
   }
   return 0;
}
Float CppiaCtx::runFloat(void *vtable)
{
   try
   {
      ((CppiaExpr *)vtable)->runVoid(this);
   }
   catch (CppiaExpr *retVal)
   {
      return retVal->runFloat(this);
   }
   return 0;
}
String CppiaCtx::runString(void *vtable)
{
   try
   {
      ((CppiaExpr *)vtable)->runVoid(this);
   }
   catch (CppiaExpr *retVal)
   {
      return retVal->runString(this);
   }
   return null();
}
void CppiaCtx::runVoid(void *vtable)
{
   try
   {
      ((CppiaExpr *)vtable)->runVoid(this);
   }
   catch (CppiaExpr *retVal)
   {
      if (retVal)
         retVal->runVoid(this);
   }
}
Dynamic CppiaCtx::runObject(void *vtable)
{
   try
   {
      ((CppiaExpr *)vtable)->runVoid(this);
   }
   catch (CppiaExpr *retVal)
   {
      return retVal->runObject(this);
   }
   return null();
}



// --- CppiaDynamicExpr ----------------------------------------
// Delegates to 'runObject'

struct CppiaDynamicExpr : public CppiaExpr
{
   CppiaDynamicExpr(const CppiaExpr *inSrc=0) : CppiaExpr(inSrc) {}

   int         runInt(CppiaCtx *ctx)    { return runObject(ctx)->__ToInt(); }
   Float       runFloat(CppiaCtx *ctx) { return runObject(ctx)->__ToDouble(); }
   ::String    runString(CppiaCtx *ctx) { return runObject(ctx)->__ToString(); }
   void        runVoid(CppiaCtx *ctx)   { runObject(ctx); }
   hx::Object *runObject(CppiaCtx *ctx) { return 0; }
};


// ------------------------------------------------------



CppiaExpr *createCppiaExpr(CppiaStream &inStream);

static void ReadExpressions(Expressions &outExpressions, CppiaStream &stream,int inN=-1)
{
   int count = inN>=0 ? inN : stream.getInt();
   outExpressions.resize(count);

   for(int i=0;i<count;i++)
      outExpressions[i] =  createCppiaExpr(stream);
}



static void LinkExpressions(Expressions &ioExpressions, CppiaData &data)
{
   for(int i=0;i<ioExpressions.size();i++)
      ioExpressions[i] = ioExpressions[i]->link(data);
}



struct CppiaFunction
{
   CppiaData &cppia;
   int       nameId;
   bool      isStatic;
   int       returnType;
   int       argCount;
   int       vtableSlot;
   bool      linked;
   std::string name;
   std::vector<ArgInfo> args;
   CppiaExpr *funExpr;

   CppiaFunction(CppiaData *inCppia,bool inIsStatic) :
      cppia(*inCppia), isStatic(inIsStatic), funExpr(0)
   {
      linked = false;
      vtableSlot = -1;
   }

   void setVTableSlot(int inSlot) { vtableSlot = inSlot; }

   void load(CppiaStream &stream,bool inExpectBody)
   {
      nameId = stream.getInt();
      name = cppia.strings[ nameId ].__s;
      returnType = stream.getInt();
      argCount = stream.getInt();
      printf("  Function %s(%d) : %s\n", name.c_str(), argCount, cppia.typeStr(returnType));
      args.resize(argCount);
      for(int a=0;a<argCount;a++)
      {
         ArgInfo arg = args[a];
         arg.nameId = stream.getInt();
         arg.optional = stream.getBool();
         arg.typeId = stream.getInt();
         printf("    arg %c%s:%s\n", arg.optional?'?':' ', cppia.identStr(arg.nameId), cppia.typeStr(arg.typeId) );
      }
      if (inExpectBody)
         funExpr = createCppiaExpr(stream);
   }
   void link( )
   {
      if (!linked)
      {
         linked = true;
         if (funExpr)
            funExpr = funExpr->link(cppia);
      }
   }
};


struct CppiaVar
{
   enum Access { accNormal, accNo, accResolve, accCall, accRequire } ;
   CppiaData *cppia;
   TypeData  *type;
   bool      isStatic;
   Access    readAccess;
   Access    writeAccess;
   int       nameId;
   int       typeId;
   int       offset;
   FieldStorage storeType;
   

   CppiaVar(CppiaData *inCppia,bool inIsStatic) :
      cppia(inCppia), isStatic(inIsStatic)
   {
      type = 0;
      nameId = 0;
      typeId = 0;
      offset = 0;
      type = 0;
      storeType = fsUnknown;
   }

   void load(CppiaStream &stream)
   {
      readAccess = getAccess(stream);
      writeAccess = getAccess(stream);
      nameId = stream.getInt();
      typeId = stream.getInt();
   }

   void link(CppiaData &cppia, int &ioOffset)
   {
      offset = ioOffset;
      type = cppia.types[typeId];
      
      switch(type->expressionType)
      {
         case etInt: ioOffset += sizeof(int); storeType=fsInt; break;
         case etFloat: ioOffset += sizeof(Float);storeType=fsFloat;  break;
         case etString: ioOffset += sizeof(String);storeType=fsString;  break;
         case etObject: ioOffset += sizeof(hx::Object *);storeType=fsObject;  break;
         case etVoid:
         case etNull:
            break;
      }
   }

   static Access getAccess(CppiaStream &stream)
   {
      std::string tok = stream.getToken();
      if (tok.size()!=1)
         throw "bad var access length";
      switch(tok[0])
      {
         case 'N': return accNormal;
         case '!': return accNo;
         case 'R': return accResolve;
         case 'C': return accCall;
         case '?': return accRequire;
      }
      throw "bad access code";
      return accNormal;
   }
};


struct CppiaConst
{
   enum Type { cInt, cFloat, cString, cBool, cNull, cThis, cSuper };

   Type type;
   int  ival;
   Float  dval;

   CppiaConst() : type(cNull), ival(0), dval(0) { }

   void fromStream(CppiaStream &stream)
   {
      std::string tok = stream.getToken();
      if (tok[0]=='i')
      {
         type = cInt;
         dval = ival = atoi(&tok[1]);
      }
      else if (tok[0]=='f')
      {
         type = cFloat;
         dval = atof(&tok[1]);
      }
      else if (tok[0]=='s')
      {
         type = cString;
         ival = atoi(&tok[1]);
      }
      else if (tok=="TRUE")
      {
         type = cInt;
         dval = ival = 1;
      }
      else if (tok=="FALSE")
      {
         type = cInt;
         dval = ival = 0;
      }
      else if (tok=="NULL")
         type = cNull;
      else if (tok=="THIS")
         type = cThis;
      else if (tok=="SUPER")
         type = cSuper;
      else
         throw "unknown const value";
   }
};


void runFunExpr(CppiaCtx *ctx, CppiaExpr *inFunExpr, hx::Object *inThis, Expressions &inArgs );

struct CppiaClass
{
   CppiaData &cppia;
   std::vector<int> implements;
   bool      isInterface;
   bool      isLinked;
   int       typeId;
   TypeData  *type;
   int       superId;
   int       extraData;
   void      **vtable;
   std::string name;

   ScriptRegistered *haxeBase;

   std::vector<CppiaFunction *> memberFunctions;
   std::vector<CppiaVar *> memberVars;

   std::vector<CppiaFunction *> staticFunctions;
   std::vector<CppiaVar *> staticVars;

   CppiaFunction *newFunc;

   CppiaClass(CppiaData &inCppia) : cppia(inCppia)
   {
      isLinked = false;
      haxeBase = 0;
      extraData = 0;
      newFunc = 0;
      isInterface = false;
      typeId = 0;
      vtable = 0;
      type = 0;
   }

   hx::Object *createInstance(CppiaCtx *ctx,Expressions &inArgs)
   {
      hx::Object *obj = haxeBase->factory(vtable,extraData);

      if (newFunc)
         runFunExpr(ctx, newFunc->funExpr, obj, inArgs );

      return obj;
   }

   CppiaExpr *findFunction(bool inStatic,int inId)
   {
      std::vector<CppiaFunction *> &funcs = inStatic ? staticFunctions : memberFunctions;
      for(int i=0;i<funcs.size();i++)
      {
         if (funcs[i]->nameId == inId)
            return funcs[i]->funExpr;
      }
      return 0;
   }

   int findFunctionSlot(int inName)
   {
      for(int i=0;i<memberFunctions.size();i++)
         if (memberFunctions[i]->nameId==inName)
            return memberFunctions[i]->vtableSlot;
      return -1;
   }

   CppiaVar *findVar(bool inStatic,int inId)
   {
      std::vector<CppiaVar *> &vars = inStatic ? staticVars : memberVars;
      for(int i=0;i<vars.size();i++)
      {
         if (vars[i]->nameId == inId)
            return vars[i];
      }
      return 0;
   }


   void load(CppiaStream &inStream)
   {
      std::string tok = inStream.getToken();

      if (tok=="CLASS")
         isInterface = false;
      else if (tok=="INTERFACE")
         isInterface = true;
      else
         throw "Bad class type";

       typeId = inStream.getInt();
       cppia.types[typeId]->cppiaClass = this;
       superId = inStream.getInt();
       int implementCount = inStream.getInt();
       implements.resize(implementCount);
       for(int i=0;i<implementCount;i++)
          implements[i] = inStream.getInt();

       name = cppia.typeStr(typeId);
       printf("Class %s\n", name.c_str());

       int fields = inStream.getInt();
       for(int f=0;f<fields;f++)
       {
          tok = inStream.getToken();
          if (tok=="FUNCTION")
          {
             bool isStatic = inStream.getStatic();
             CppiaFunction *func = new CppiaFunction(&cppia,isStatic);
             if (isStatic)
                staticFunctions.push_back(func);
             else
                memberFunctions.push_back(func);
             func->load(inStream,!isInterface);
          }
          else if (tok=="VAR")
          {
             bool isStatic = inStream.getStatic();
             CppiaVar *var = new CppiaVar(&cppia,isStatic);
             if (isStatic)
                staticVars.push_back(var);
             else
                memberVars.push_back(var);
             var->load(inStream);
          }
          else if (tok=="INLINE")
          {
             // OK
          }
          else
             throw "unknown field type";
       }
   }

   void linkTypes()
   {
      if (isLinked)
         return;
      isLinked = true;

      type = cppia.types[typeId];
      TypeData *superType = superId ? cppia.types[ superId ] : 0;
      CppiaClass  *cppiaSuper = superType ? superType->cppiaClass : 0;
      if (superType && superType->cppiaClass)
         superType->cppiaClass->linkTypes();

      printf(" Linking class '%s' ", type->name.__s);
      if (!superType)
         printf("script base\n");
      else if (cppiaSuper)
         printf("extends script '%s'\n", superType->name.__s);
      else
         printf("extends haxe '%s'\n", superType->name.__s);

      // Combine member vars ...
      if (cppiaSuper)
      {
         std::vector<CppiaVar *> combinedVars(cppiaSuper->memberVars );
         for(int i=0;i<memberVars.size();i++)
         {
           for(int j=0;j<combinedVars.size();j++)
              if (combinedVars[j]->nameId==memberVars[i]->nameId)
                printf("Warning duplicate member var %s\n", cppia.strings[memberVars[i]->nameId].__s);
            combinedVars.push_back(memberVars[i]);
         }
         memberVars.swap(combinedVars);
      }

      // Combine member functions ...
      if (cppiaSuper)
      {
         std::vector<CppiaFunction *> combinedFunctions(cppiaSuper->memberFunctions );
         for(int i=0;i<memberFunctions.size();i++)
         {
            bool found = false;
            for(int j=0;j<combinedFunctions.size();j++)
               if (combinedFunctions[j]->name==memberFunctions[i]->name)
               {
                  combinedFunctions[j] = memberFunctions[i];
                  found = true;
                  break;
               }
            if (!found)
               combinedFunctions.push_back(memberFunctions[i]);
            memberFunctions.swap(combinedFunctions);
         }
      }


      haxeBase = type->haxeBase;

      // Calculate table offsets...
      int d0 = haxeBase->mDataOffset;
      printf("  base haxe size %s = %d\n", haxeBase->name.c_str(), d0);
      int offset = d0;
      for(int i=0;i<memberVars.size();i++)
      {
         printf("   link var %s @ %d\n", cppia.identStr(memberVars[i]->nameId), offset);
         memberVars[i]->link(cppia,offset);
      }
      extraData = offset - d0;
      printf("  script member vars size = %d\n", extraData);

      // Combine vtable positions...
      printf("  format haxe callable vtable....\n");
      std::vector<std::string> table;
      haxeBase->addVtableEntries(table);
      for(int i=0;i<table.size();i++)
         printf("   table[%d] = %s\n", i, table[i].c_str() );

      int vtableSlot = table.size();
      for(int i=0;i<memberFunctions.size();i++)
      {
         int idx = -1;
         for(int j=0;j<table.size();j++)
            if (table[j] == memberFunctions[i]->name)
            {
               idx = j;
               break;
            }
         if (idx<0)
         {
            idx = vtableSlot++;
            printf("   cppia slot [%d] = %s\n", idx, memberFunctions[i]->name.c_str() );
         }
         else
            printf("   override slot [%d] = %s\n", idx, memberFunctions[i]->name.c_str() );
         memberFunctions[i]->setVTableSlot(idx);
      }
      vtable = new void*[vtableSlot + 1];
      memset(vtable, 0, sizeof(void *)*(vtableSlot+1));
      *vtable++ = this;
      printf("  vtable size %d -> %p\n", vtableSlot, vtable);

      // Extract contruct function ...
      for(int i=0;i<staticFunctions.size();i++)
      {
         if (staticFunctions[i]->name == "new")
         {
            newFunc = staticFunctions[i];
            staticFunctions.erase( staticFunctions.begin() + i);
            break;
         }
      }

      if (!newFunc && cppiaSuper && cppiaSuper->newFunc)
         throw "No chaining constructor";

      printf("  this constructor %p\n", newFunc);
   }

   void link()
   {
      int newPos = -1;
      for(int i=0;i<staticFunctions.size();i++)
      {
         staticFunctions[i]->link();
      }
      if (newFunc)
      {
         newFunc->link();
      }

      for(int i=0;i<memberFunctions.size();i++)
         memberFunctions[i]->link();

      for(int i=0;i<memberFunctions.size();i++)
         vtable[ memberFunctions[i]->vtableSlot ] = memberFunctions[i]->funExpr;


      //printf("Found haxeBase %s = %p / %d\n", cppia.types[typeId]->name.__s, haxeBase, dataSize );
   }
};

hx::Object *createClosure(CppiaCtx *ctx, struct FunExpr *inFunction);

static String sInvalidArgCount = HX_CSTRING("Invalid arguement count");

struct FunExpr : public CppiaExpr
{
   int returnType;
   int argCount;
   int stackSize;
   
   std::vector<CppiaStackVar> args;
   std::vector<bool>          hasDefault;
   std::vector<CppiaConst>    initVals;
   CppiaExpr *body;

   std::vector<CppiaStackVar *> captureVars;
   int                          captureSize;

   FunExpr(CppiaStream &stream)
   {
      body = 0;
      stackSize = 0;
      returnType = stream.getInt();
      argCount = stream.getInt();
      args.resize(argCount);
      hasDefault.resize(argCount);
      captureSize = 0;
      for(int a=0;a<argCount;a++)
      {
         args[a].fromStream(stream);
         bool init = stream.getBool();
         hasDefault.push_back(init);
         if (init)
            initVals[a].fromStream(stream);
      }
      body = createCppiaExpr(stream);
   }

   CppiaExpr *link(CppiaData &inData)
   {
      StackLayout *oldLayout = inData.layout;
      StackLayout layout(oldLayout);
      inData.layout = &layout;

      for(int a=0;a<args.size();a++)
         args[a].link(inData);

      body = body->link(inData);

      captureVars.swap(layout.captureVars);
      captureSize = layout.captureSize;

      stackSize = layout.size;
      inData.layout = oldLayout;
      return this;
   }

   void pushArgs(CppiaCtx *ctx, hx::Object *inThis, Expressions &inArgs)
   {
      if (argCount!=inArgs.size())
      {
         printf("Arg count mismatch?\n");
         return;
      }

      ctx->push( inThis );

      for(int a=0;a<argCount;a++)
      {
         CppiaStackVar &var = args[a];
         // TODO capture
         if (hasDefault[a])
         {
            hx::Object *obj = inArgs[a]->runObject(ctx);
            switch(var.expressionType)
            {
               case etInt:
                  ctx->pushInt( obj ? obj->__ToInt() : initVals[a].ival );
                  break;
               case etFloat:
                  ctx->pushFloat( (Float)(obj ? obj->__ToDouble() : initVals[a].dval) );
                  break;
               /* todo - default strings.
               case etString:
                  ctx.push( obj ? obj->__ToString() : initVals[a].ival );
                  break;
               */
               default:
                  ctx->pushObject(obj);
            }
         }
         else
         {
            switch(var.expressionType)
            {
               case etInt:
                  ctx->pushInt(inArgs[a]->runInt(ctx));
                  break;
               case etFloat:
                  ctx->pushFloat(inArgs[a]->runFloat(ctx));
                  break;
               case etString:
                  ctx->pushString(inArgs[a]->runString(ctx));
                  break;
               default:
                  ctx->pushObject(inArgs[a]->runObject(ctx));
            }
         }
      }
   }

   // Return the closure
   hx::Object *runObject(CppiaCtx *ctx)
   {
      return createClosure(ctx,this);

   }

   String runString(CppiaCtx *ctx) { return HX_CSTRING("#function"); }

   // Run the actual function
   void runVoid(CppiaCtx *ctx)
   {
      if (stackSize)
      {
         memset(ctx->pointer, 0 , stackSize );
         ctx->pointer += stackSize;
      }
      body->runVoid(ctx);
   }

   void addStackVarsSpace(CppiaCtx *ctx)
   {
      if (stackSize)
      {
         memset(ctx->pointer, 0 , stackSize );
         ctx->pointer += stackSize;
      }
   }

   
   bool pushDefault(CppiaCtx *ctx,int arg)
   {
      if (!hasDefault[arg])
         return false;

      switch(args[arg].expressionType)
      {
         case etInt:
            ctx->pushInt( initVals[arg].ival );
            break;
         case etFloat:
            ctx->pushFloat( initVals[arg].dval );
            break;
               /* todo - default strings.
               case etString:
                  ctx.push( obj ? obj->__ToString() : initVals[a].ival );
                  break;
               */
         default:
            printf("Unimplemented default value\n");
      }
      return true;
   }

   void addExtraDefaults(CppiaCtx *ctx,int inHave)
   {
      if (inHave>argCount)
         throw sInvalidArgCount;

      for(int a=inHave;a<argCount;a++)
      {
         CppiaStackVar &var = args[a];
         if (!pushDefault(ctx,a))
            throw sInvalidArgCount;
      }
   }

};


class CppiaClosure : public hx::Object
{
public:
   inline void *operator new( size_t inSize, int inExtraDataSize )
     { return hx::InternalNew(inSize + inExtraDataSize,true); }
   inline void operator delete(void *,int) {}

   FunExpr *function;

   CppiaClosure(CppiaCtx *ctx, FunExpr *inFunction)
   {
      function = inFunction;

      unsigned char *base = ((unsigned char *)this) + sizeof(CppiaClosure);

      *(hx::Object **)base = ctx->getThis();

      for(int i=0;i<function->captureVars.size();i++)
      {
         CppiaStackVar *var = function->captureVars[i];
         int size = sTypeSize[var->expressionType];
         memcpy( base+var->capturePos, ctx->frame + var->fromStackPos, size );
      }
   }

   // Create member closure...
   CppiaClosure(hx::Object *inThis, FunExpr *inFunction)
   {
      function = inFunction;

      unsigned char *base = ((unsigned char *)this) + sizeof(CppiaClosure);

      *(hx::Object **)base = inThis;
   }


   Dynamic doRun(CppiaCtx *ctx, int inHaveArgs)
   {
      function->addExtraDefaults(ctx,inHaveArgs);
      function->addStackVarsSpace(ctx);

      unsigned char *base = ((unsigned char *)this) + sizeof(CppiaClosure);
      *(hx::Object **)ctx->frame =  *(hx::Object **)base;

      for(int i=0;i<function->captureVars.size();i++)
      {
         CppiaStackVar *var = function->captureVars[i];
         int size = sTypeSize[var->expressionType];
         memcpy( ctx->frame+var->stackPos, base + var->capturePos, size);
      }

      try {
         function->body->runVoid(ctx);
      }
      catch (CppiaExpr *retVal)
      {
         if (retVal)
            return retVal->runObject(ctx);
      }
      return null();
   }

   void pushArg(CppiaCtx *ctx, int a, Dynamic inValue)
   {
      if (!inValue.mPtr && function->pushDefault(ctx,a) )
          return;

      switch(function->args[a].expressionType)
      {
         case etInt:
            ctx->pushInt(inValue->__ToInt());
            return;
         case etFloat:
            ctx->pushFloat(inValue->__ToDouble());
            break;
         case etString:
            ctx->pushString(inValue->toString());
            break;
         default:
            ctx->pushObject(inValue.mPtr);
      }
   }



   Dynamic __Run(const Array<Dynamic> &inArgs)
   {
      CppiaCtx *ctx = CppiaCtx::getCurrent();

      AutoStack a(ctx);
      ctx->pointer += sizeof(hx::Object *);

      int haveArgs = inArgs->length;
      if (haveArgs>function->argCount)
         throw sInvalidArgCount;

      for(int a=0; a<haveArgs; a++)
         pushArg(ctx,a,inArgs[a]);

      return doRun(ctx,haveArgs);
   }

   Dynamic __run()
   {
      CppiaCtx *ctx = CppiaCtx::getCurrent();
      AutoStack a(ctx);
      ctx->pointer += sizeof(hx::Object *);
      return doRun(ctx,0);
   }

   Dynamic __run(D a)
   {
      CppiaCtx *ctx = CppiaCtx::getCurrent();
      AutoStack aut(ctx);
      ctx->pointer += sizeof(hx::Object *);
      pushArg(ctx,0,a);
      return doRun(ctx,1);
   }
   Dynamic __run(D a,D b)
   {
      CppiaCtx *ctx = CppiaCtx::getCurrent();
      AutoStack aut(ctx);
      ctx->pointer += sizeof(hx::Object *);
      pushArg(ctx,0,a);
      pushArg(ctx,1,b);
      return doRun(ctx,2);
   }
   Dynamic __run(D a,D b,D c)
   {
      CppiaCtx *ctx = CppiaCtx::getCurrent();
      AutoStack aut(ctx);
      ctx->pointer += sizeof(hx::Object *);
      pushArg(ctx,0,a);
      pushArg(ctx,1,b);
      pushArg(ctx,2,c);
      return doRun(ctx,3);
   }
   Dynamic __run(D a,D b,D c,D d)
   {
      CppiaCtx *ctx = CppiaCtx::getCurrent();
      AutoStack aut(ctx);
      ctx->pointer += sizeof(hx::Object *);
      pushArg(ctx,0,a);
      pushArg(ctx,1,b);
      pushArg(ctx,2,c);
      pushArg(ctx,3,d);
      return doRun(ctx,4);

   }
   Dynamic __run(D a,D b,D c,D d,D e)
   {
      CppiaCtx *ctx = CppiaCtx::getCurrent();
      AutoStack aut(ctx);
      ctx->pointer += sizeof(hx::Object *);
      pushArg(ctx,0,a);
      pushArg(ctx,1,b);
      pushArg(ctx,2,c);
      pushArg(ctx,3,d);
      pushArg(ctx,4,e);
      return doRun(ctx,5);
   }

   int __GetType() const { return vtFunction; }
   int __ArgCount() const { return function->args.size(); }

   String toString() { return HX_CSTRING("function"); }
   // TODO - mark , move
};


hx::Object *createClosure(CppiaCtx *ctx, FunExpr *inFunction)
{
   return new (inFunction->captureSize) CppiaClosure(ctx,inFunction);
}




void runFunExpr(CppiaCtx *ctx, CppiaExpr *inFunExpr, hx::Object *inThis, Expressions &inArgs )
{
   unsigned char *pointer = ctx->pointer;
   ((FunExpr *)inFunExpr)->pushArgs(ctx, inThis, inArgs);
   AutoStack save(ctx,pointer);
   try {
      inFunExpr->runVoid(ctx);
   }
   catch (CppiaExpr *retVal) { }
}

struct BlockExpr : public CppiaExpr
{
   Expressions expressions;

   BlockExpr(CppiaStream &stream)
   {
      ReadExpressions(expressions,stream);
   }

   CppiaExpr *link(CppiaData &data)
   {
      LinkExpressions(expressions,data);
      return this;
   }

   virtual ExprType getType()
   {
      if (expressions.size()==0)
         return etNull;
      return expressions[expressions.size()-1]->getType();
   }

   #define BlockExprRun(ret,name,defVal) \
     ret name(CppiaCtx *ctx) \
     { \
        for(int a=0;a<expressions.size()-1;a++) \
          expressions[a]->runVoid(ctx); \
        if (expressions.size()>0) \
           return expressions[expressions.size()-1]->name(ctx); \
        return defVal; \
     }
   BlockExprRun(int,runInt,0)
   BlockExprRun(Float ,runFloat,0)
   BlockExprRun(String,runString,null())
   BlockExprRun(hx::Object *,runObject,0)
   void  runVoid(CppiaCtx *ctx)
   {
      for(int a=0;a<expressions.size();a++)
         expressions[a]->runVoid(ctx);
   }
};

struct IfElseExpr : public CppiaExpr
{
   CppiaExpr *condition;
   CppiaExpr *doIf;
   CppiaExpr *doElse;

   IfElseExpr(CppiaStream &stream)
   {
      condition = createCppiaExpr(stream);
      doIf = createCppiaExpr(stream);
      doElse = createCppiaExpr(stream);
   }
};


struct IfExpr : public CppiaExpr
{
   CppiaExpr *condition;
   CppiaExpr *doIf;

   IfExpr(CppiaStream &stream)
   {
      condition = createCppiaExpr(stream);
      doIf = createCppiaExpr(stream);
   }
};


struct IsNull : public CppiaExpr
{
   CppiaExpr *condition;

   IsNull(CppiaStream &stream) { condition = createCppiaExpr(stream); }
};


struct IsNotNull : public CppiaExpr
{
   CppiaExpr *condition;

   IsNotNull(CppiaStream &stream) { condition = createCppiaExpr(stream); }
};


struct CallFunExpr : public CppiaExpr
{
   Expressions args;
   CppiaExpr   *thisExpr;
   FunExpr     *function;
   ExprType    returnType;

   CallFunExpr(const CppiaExpr *inSrc, CppiaExpr *inThisExpr, FunExpr *inFunction, Expressions &ioArgs )
      : CppiaExpr(inSrc)
   {
      args.swap(ioArgs);
      function = inFunction;
      thisExpr = inThisExpr;
   }

   CppiaExpr *link(CppiaData &inData)
   {
      LinkExpressions(args,inData);
      // Should already be linked
      //function = (FunExpr *)function->link(inData);
      if (thisExpr)
         thisExpr = thisExpr->link(inData);
      returnType = inData.types[ function->returnType ]->expressionType;
      return this;
   }

   ExprType getType() { return returnType; }

   #define CallFunExprVal(ret,funcName,def) \
   ret funcName(CppiaCtx *ctx) \
   { \
      unsigned char *pointer = ctx->pointer; \
      function->pushArgs(ctx,thisExpr?thisExpr->runObject(ctx):0,args); \
      AutoStack save(ctx,pointer); \
      try \
      { \
         function->runVoid(ctx); \
      } \
      catch (CppiaExpr *retVal) \
      { \
         if (retVal) \
            return retVal->funcName(ctx); \
      } \
      return def; \
   }
   CallFunExprVal(int,runInt,0);
   CallFunExprVal(Float ,runFloat,0);
   CallFunExprVal(String,runString,null());
   CallFunExprVal(hx::Object *,runObject,0);

   void runVoid(CppiaCtx *ctx)
   {
      unsigned char *pointer = ctx->pointer;
      function->pushArgs(ctx,thisExpr?thisExpr->runObject(ctx):ctx->getThis(),args);
      AutoStack save(ctx,pointer);
      try { function->runVoid(ctx); }
      catch (CppiaExpr *retVal)
      {
         if (retVal)
            retVal->runVoid(ctx);
      }
   }

};

// ---


struct CppiaExprWithValue : public CppiaExpr
{
   Dynamic     value;

   CppiaExprWithValue(const CppiaExpr *inSrc=0) : CppiaExpr(inSrc)
   {
      value.mPtr = 0;
   }

   hx::Object *runObject(CppiaCtx *ctx) { return value.mPtr; }
   void mark(hx::MarkContext *__inCtx) { HX_MARK_MEMBER(value); }
   void visit(hx::VisitContext *__inCtx) { HX_VISIT_MEMBER(value); }

   CppiaExpr *link(CppiaData &inData)
   {
      inData.markable.push_back(this);
      return this;
   }

   void runVoid(CppiaCtx *ctx) { runObject(ctx); }

};

// ---



struct CallDynamicFunction : public CppiaExprWithValue
{
   Expressions args;
   Dynamic     value;

   CallDynamicFunction(CppiaData &inData, const CppiaExpr *inSrc,
                       Dynamic inFunction, Expressions &ioArgs )
      : CppiaExprWithValue(inSrc)
   {
      args.swap(ioArgs);
      value = inFunction;
      inData.markable.push_back(this);
   }

   CppiaExpr *link(CppiaData &inData)
   {
      LinkExpressions(args,inData);
      return this;
   }

   ExprType getType() { return etObject; }

   hx::Object *runObject(CppiaCtx *ctx)
   {
      int n = args.size();
      switch(n)
      {
         case 0:
            return value().mPtr;
         case 1:
            {
               Dynamic arg0( args[0]->runObject(ctx) );
               return value->__run(arg0).mPtr;
            }
         case 2:
            {
               Dynamic arg0( args[0]->runObject(ctx) );
               Dynamic arg1( args[1]->runObject(ctx) );
               return value->__run(arg0,arg1).mPtr;
            }
         case 3:
            {
               Dynamic arg0( args[0]->runObject(ctx) );
               Dynamic arg1( args[1]->runObject(ctx) );
               Dynamic arg2( args[2]->runObject(ctx) );
               return value->__run(arg0,arg1,arg2).mPtr;
            }
         case 4:
            {
               Dynamic arg0( args[0]->runObject(ctx) );
               Dynamic arg1( args[1]->runObject(ctx) );
               Dynamic arg2( args[2]->runObject(ctx) );
               Dynamic arg3( args[3]->runObject(ctx) );
               return value->__run(arg0,arg1,arg2,arg3).mPtr;
            }
         case 5:
            {
               Dynamic arg0( args[0]->runObject(ctx) );
               Dynamic arg1( args[1]->runObject(ctx) );
               Dynamic arg2( args[2]->runObject(ctx) );
               Dynamic arg3( args[3]->runObject(ctx) );
               Dynamic arg4( args[4]->runObject(ctx) );
               return value->__run(arg0,arg1,arg2,arg3,arg4).mPtr;
            }
      }

      Array<Dynamic> argVals = Array_obj<Dynamic>::__new(n,n);
      for(int a=0;a<n;a++)
         argVals[a] = Dynamic( args[a]->runObject(ctx) );
      return value->__Run(argVals).mPtr;
   }

   int runInt(CppiaCtx *ctx)
   {
      hx::Object *result = runObject(ctx);
      return result ? result->__ToInt() : 0;
   }
   Float  runFloat(CppiaCtx *ctx)
   {
      hx::Object *result = runObject(ctx);
      return result ? result->__ToDouble() : 0;
   }
   String runString(CppiaCtx *ctx)
   {
      hx::Object *result = runObject(ctx);
      return result ? result->__ToString() : 0;
   }
};

struct SetExpr : public CppiaExpr
{
   int toTypeId;
   int fromTypeId;
   CppiaExpr *lvalue;
   CppiaExpr *value;

   SetExpr(CppiaStream &stream)
   {
      toTypeId = stream.getInt();
      fromTypeId = stream.getInt();
      lvalue = createCppiaExpr(stream);
      value = createCppiaExpr(stream);
   }

   CppiaExpr *link(CppiaData &inData)
   {
      lvalue = lvalue->link(inData);
      value = value->link(inData);
      // TODO cast
      CppiaExpr *result = lvalue->makeSetter(value);
      delete this;
      return result;
   }

};

struct NewExpr : public CppiaExpr
{
   int classId;
   TypeData *type;
   Expressions args;
	hx::ConstructArgsFunc constructor;


   NewExpr(CppiaStream &stream)
   {
      classId = stream.getInt();
      constructor = 0;
      ReadExpressions(args,stream);
   }

   CppiaExpr *link(CppiaData &inData)
   {
      type = inData.types[classId];
      if (!type->cppiaClass && type->haxeClass.mPtr)
         constructor = type->haxeClass.mPtr->mConstructArgs;

      LinkExpressions(args,inData);
      return this;
   }
   hx::Object *runObject(CppiaCtx *ctx)
   {
      if (type->arrayType)
      {
         int size = 0;
         if (args.size()==1)
            size = args[0]->runInt(ctx);
         switch(type->arrayType)
         {
            case arrBool:
               return Array_obj<bool>::__new(size,size).mPtr;
            case arrUnsignedChar:
               return Array_obj<unsigned char>::__new(size,size).mPtr;
            case arrInt:
               return Array_obj<int>::__new(size,size).mPtr;
            case arrFloat:
               return Array_obj<Float>::__new(size,size).mPtr;
            case arrString:
               return Array_obj<String>::__new(size,size).mPtr;
            case arrDynamic:
               return Array_obj<Dynamic>::__new(size,size).mPtr;
            default:
               return 0;
         }
      }
      else if (constructor)
      {
         int n = args.size();
         printf("Run constructor %d\n", n);
         Array< Dynamic > argList = Array_obj<Dynamic>::__new(n,n);
         for(int a=0;a<n;a++)
            argList[a].mPtr = args[a]->runObject(ctx);
            
          return constructor(argList).mPtr;
      }
      else
      {
         return type->cppiaClass->createInstance(ctx,args);
      }

      printf("Can't create non haxe type\n");
      return 0;
   }
   
};

template<typename T>
inline void SetVal(null &out, const T &value) {  }
template<typename T>
inline void SetVal(int &out, const T &value) { out = value; }
inline void SetVal(int &out, const String &value) { out = 0; }
template<typename T>
inline void SetVal(Float &out, const T &value) { out = value; }
inline void SetVal(Float &out, const String &value) { out = 0; }
template<typename T>
inline void SetVal(String &out, const T &value) { out = String(value); }
template<typename T>
inline void SetVal(Dynamic &out, const T &value) { out = value; }

//template<typname RETURN>
struct CallHaxe : public CppiaExpr
{
   Expressions args;
   CppiaExpr *thisExpr;
   ScriptFunction function;

   CallHaxe(CppiaExpr *inSrc,ScriptFunction inFunction, CppiaExpr *inThis, Expressions &ioArgs )
       : CppiaExpr(inSrc)
   {
      args.swap(ioArgs);
      thisExpr = inThis;
      function = inFunction;
   }
   CppiaExpr *link(CppiaData &inData)
   {
      if (strlen(function.signature) != args.size()+1)
         throw "CallHaxe: Invalid arg count";
      for(int i=0;i<args.size()+1;i++)
      {
         switch(function.signature[i])
         {
            case sigInt: case sigFloat: case sigString: case sigObject:
               break; // Ok
            case sigVoid: 
               if (i==0) // return void ok
                  break;
               // fallthough
            default:
               throw "Bad haxe signature";
         }
      }

      if (thisExpr)
         thisExpr = thisExpr->link(inData);
      LinkExpressions(args,inData);

      return this;
   }


   template<typename T>
   void run(CppiaCtx *ctx,T &outValue)
   {
      unsigned char *pointer = ctx->pointer;
      ctx->pushObject(thisExpr ? thisExpr->runObject(ctx) : ctx->getThis());

      const char *s = function.signature+1;
      for(int a=0;a<args.size();a++)
      {
         CppiaExpr *arg = args[a];
         switch(*s++)
         {
            case sigInt: ctx->pushInt( arg->runInt(ctx) ); break;
            case sigFloat: ctx->pushFloat( arg->runFloat(ctx) ); break;
            case sigString: ctx->pushString( arg->runString(ctx) ); break;
            case sigObject: ctx->pushObject( arg->runObject(ctx) ); break;
            default: ;// huh?
         }
      }

      AutoStack a(ctx,pointer);
      //printf("Execute %p %p\n", function.execute, ctx->getThis());
      function.execute(ctx);

      if (sizeof(outValue)>0)
      {
         if (function.signature[0]==sigInt)
            SetVal(outValue,ctx->getInt());
         else if (function.signature[0]==sigFloat)
            SetVal(outValue,ctx->getFloat());
         else if (function.signature[0]==sigString)
            SetVal(outValue,ctx->getString());
         else if (function.signature[0]==sigObject)
            SetVal(outValue,ctx->getObject());
      }
   }

   void runVoid(CppiaCtx *ctx)
   {
      null val;
      run(ctx,val);
   }
   int runInt(CppiaCtx *ctx)
   {
      int val;
      run(ctx,val);
      return val;
   }
   Float runFloat(CppiaCtx *ctx)
   {
      Float val;
      run(ctx,val);
      return val;
   }
   String runString(CppiaCtx *ctx)
   {
      String val;
      run(ctx,val);
      return val;
   }
   hx::Object *runObject(CppiaCtx *ctx)
   {
      Dynamic val;
      run(ctx,val);
      return val.mPtr;
   }
};

struct CallStatic : public CppiaExpr
{
   int classId;
   int fieldId;
   Expressions args;
  
   CallStatic(CppiaStream &stream)
   {
      classId = stream.getInt();
      fieldId = stream.getInt();
      ReadExpressions(args,stream);
   }

   CppiaExpr *link(CppiaData &inData)
   {

      TypeData *type = inData.types[classId];
      String field = inData.strings[fieldId];

      if (type->cppiaClass)
      {
         FunExpr *func = (FunExpr *)type->cppiaClass->findFunction(true,fieldId);
         if (!func)
         {
            printf("Could not find static function %s in %s\n", field.__s, type->name.__s);
         }
         else
         {
            CppiaExpr *replace = new CallFunExpr( this, 0, func, args );
            replace->link(inData);
            delete this;
            return replace;
         }
      }

      if (type->haxeClass.mPtr)
      {
         // TODO - might change if dynamic function (eg, trace)
         Dynamic func = type->haxeClass.mPtr->__Field( field, false );
         if (func.mPtr)
         {
            CppiaExpr *replace = new CallDynamicFunction(inData, this, func, args );
            delete this;
            replace->link(inData);
            return replace;
         }
      }

      printf("Unknown call to %s::%s\n", type->name.__s, field.__s);
      throw "Bad link";
      return this;
   }
};





struct CallMemberVTable : public CppiaExpr
{
   Expressions args;
   CppiaExpr   *thisExpr;
   int         slot;
   ExprType    returnType;

   CallMemberVTable(CppiaExpr *inSrc, CppiaExpr *inThis, int inVTableSlot, Expressions &ioArgs)
      : CppiaExpr(inSrc)
   {
      args.swap(ioArgs);
      slot = inVTableSlot;
      thisExpr = inThis;
   }
   CppiaExpr *link(CppiaData &inData)
   {
      if (thisExpr)
         thisExpr = thisExpr->link(inData);
      LinkExpressions(args,inData);
      return this;
   }

   #define CALL_VTABLE_SETUP \
      hx::Object *thisVal = thisExpr ? thisExpr->runObject(ctx) : ctx->getThis(); \
      FunExpr **vtable = (FunExpr **)thisVal->__GetScriptVTable(); \
      unsigned char *pointer = ctx->pointer; \
      vtable[slot]->pushArgs(ctx, thisVal, args); \
      AutoStack save(ctx,pointer); \
      try { \
         vtable[slot]->runVoid(ctx); \
      }

   void runVoid(CppiaCtx *ctx)
   {
      CALL_VTABLE_SETUP
      catch (CppiaExpr *retVal) { }
   }
   int runInt(CppiaCtx *ctx)
   {
      CALL_VTABLE_SETUP
      catch (CppiaExpr *retVal) { return retVal->runFloat(ctx); }
      return 0;
   }
 
   Float runFloat(CppiaCtx *ctx)
   {
      CALL_VTABLE_SETUP
      catch (CppiaExpr *retVal) { return retVal->runFloat(ctx); }
      return 0.0;
   }
   String runString(CppiaCtx *ctx)
   {
      CALL_VTABLE_SETUP
      catch (CppiaExpr *retVal) { return retVal->runString(ctx); }
      return null();
   }
   hx::Object *runObject(CppiaCtx *ctx)
   {
      CALL_VTABLE_SETUP
      catch (CppiaExpr *retVal) { return retVal->runObject(ctx); }
      return 0;
   }


};

template<typename T>
struct CallArray : public CppiaExpr
{
   Expressions args;
   CppiaExpr   *thisExpr;
   std::string funcName;

   CallArray(const CppiaExpr *inSrc, CppiaExpr *inThisExpr, String inFunc, Expressions &ioArgs )
      : CppiaExpr(inSrc)
   {
      args.swap(ioArgs);
      thisExpr = inThisExpr;
      funcName = inFunc.__s;
   }

   CppiaExpr *link(CppiaData &inData)
   {
      LinkExpressions(args,inData);
      if (thisExpr)
         thisExpr = thisExpr->link(inData);
      return this;
   }

   void runVoid(CppiaCtx *ctx)
   {
      Array_obj<T> *ptr = (Array_obj<T> *) thisExpr->runObject(ctx);
      if (funcName=="push")
      {
         ptr->push( Dynamic(args[0]->runObject(ctx) ) );
      }
   }
};

struct CallMember : public CppiaExpr
{
   int classId;
   int fieldId;
   CppiaExpr *thisExpr;
   Expressions args;
  
   CallMember(CppiaStream &stream,bool isThisCall=false, bool isSuperCall=false)
   {
      classId = stream.getInt();
      fieldId = isSuperCall ? 0 : stream.getInt();
      int n = stream.getInt();
      thisExpr = isThisCall ? 0 : createCppiaExpr(stream);
      ReadExpressions(args,stream,n);
   }

   CppiaExpr *linkSuperCall(CppiaData &inData)
   {
      TypeData *type = inData.types[classId];

      if (type->cppiaClass)
      {
         printf("Using cppia super %p %p\n", type->cppiaClass->newFunc, type->cppiaClass->newFunc->funExpr);
         CppiaExpr *replace = new CallFunExpr( this, 0, (FunExpr*)type->cppiaClass->newFunc->funExpr, args );
         replace->link(inData);
         delete this;
         return replace;
      }
      printf("Using haxe super\n");
      ScriptRegistered *superReg = (*sScriptRegistered)[type->name.__s];
      if (!superReg)
      {
         printf("No class registered for %s\n", type->name.__s);
         throw "Unknown super call";
      }
      if (!superReg->construct.execute)
      {
         printf("Call super - nothing to do...\n");
         CppiaExpr *replace = new CppiaExpr(this);
         delete this;
         return replace;
      }
      CppiaExpr *replace = new CallHaxe( this, superReg->construct,0, args );
      replace->link(inData);
      delete this;
      return replace;
   }



   CppiaExpr *link(CppiaData &inData)
   {
      if (fieldId==0)
         return linkSuperCall(inData);

      TypeData *type = inData.types[classId];
      String field = inData.strings[fieldId];
      printf("Linking %s::%s\n", type->name.__s, field.__s);

      CppiaExpr *replace = 0;
      // TODO - string functions
      if (type->arrayType)
      {
         replace = createArrayBuiltin(this, type->arrayType, thisExpr, field, args);
      }

      if (type->cppiaClass && !replace)
      {
         int vtableSlot = type->cppiaClass->findFunctionSlot(fieldId);
         //printf(" vslot %d\n", vtableSlot);
         if (vtableSlot>=0)
         {
            replace = new CallMemberVTable( this, thisExpr, vtableSlot, args );
         }
      }
      if (type->haxeBase && !replace)
      {
         ScriptFunction func = type->haxeBase->findFunction(field.__s);
         if (func.signature)
         {
            //printf(" found function %s\n", func.signature );
            replace = new CallHaxe( this, func, thisExpr, args );
         }
      }

      if (replace)
      {
         replace->link(inData);
         delete this;
         return replace;
      }

      printf("Could not link %s::%s\n", type->name.c_str(), field.__s );

      return this;
   }
};




struct Call : public CppiaExpr
{
   Expressions args;
   CppiaExpr   *func;
  
   Call(CppiaStream &stream)
   {
      int argCount = stream.getInt();
      func = createCppiaExpr(stream);
      ReadExpressions(args,stream,argCount);
   }
   CppiaExpr *link(CppiaData &inData)
   {
      func = func->link(inData);
      LinkExpressions(args,inData);
      return this;
   }
   hx::Object *runObject(CppiaCtx *ctx)
   {
      hx::Object *funcVal = func->runObject(ctx);
      int size = args.size();
      Array<Dynamic> argArray = Array_obj<Dynamic>::__new(size,size);
      for(int s=0;s<size;s++)
         argArray[s] = args[s]->runObject(ctx);

      return funcVal->__Run(argArray).mPtr;
   }
   int runInt(CppiaCtx *ctx) { return runObject(ctx)->__ToInt(); }
   Float runFloat(CppiaCtx *ctx) { return runObject(ctx)->__ToDouble(); }
   String runString(CppiaCtx *ctx) { return runObject(ctx)->__ToString(); }
   void runVoid(CppiaCtx *ctx) { runObject(ctx); }
};


struct GetFieldByName : public CppiaDynamicExpr
{
   int         nameId;
   int         classId;
   int         vtableSlot;
   CppiaExpr   *object;
   String      name;
  
   GetFieldByName(CppiaStream &stream,bool isThisObject)
   {
      classId = stream.getInt();
      nameId = stream.getInt();
      object = isThisObject ? 0 : createCppiaExpr(stream);
      name.__s = 0;
      vtableSlot = -1;
   }
   GetFieldByName(const CppiaExpr *inSrc, int inNameId, CppiaExpr *inObject)
      : CppiaDynamicExpr(inSrc)
   {
      nameId = inNameId;
      object = inObject;
      name.__s = 0;
   }
   CppiaExpr *link(CppiaData &inData)
   {
      if (object)
         object = object->link(inData);
      TypeData *type = inData.types[classId];
      if (type->cppiaClass)
         vtableSlot  = type->cppiaClass->findFunctionSlot(nameId);

      // Use runtime lookup...
      if (!vtableSlot)
      {
         name = inData.strings[nameId];
         inData.markable.push_back(this);
      }
      return this;
   }

   hx::Object *runObject(CppiaCtx *ctx)
   {
      hx::Object *instance = object ? object->runObject(ctx) : ctx->getThis();
      if (vtableSlot>=0)
      {
         FunExpr **vtable = (FunExpr **)instance->__GetScriptVTable();
         FunExpr *func = vtable[vtableSlot];
         return new (sizeof(hx::Object *)) CppiaClosure(instance, func);
      }
      return instance->__Field(name,true).mPtr;
   }

   void mark(hx::MarkContext *__inCtx)
   {
      HX_MARK_MEMBER(name);
   }
   void visit(hx::VisitContext *__inCtx)
   {
      HX_VISIT_MEMBER(name);
   }

};



template<typename T, int REFMODE> 
struct MemReference : public CppiaExpr
{
   int offset;
   CppiaExpr *object;

   #define MEMGETVAL \
     *(T *)( \
         ( REFMODE==locObj ? (char *)object->runObject(ctx) : \
           REFMODE==locThis ?(char *)ctx->getThis() : \
                             (char *)ctx->frame \
         ) + offset )

   MemReference(const CppiaExpr *inSrc, int inOffset, CppiaExpr *inExpr=0)
      : CppiaExpr(inSrc)
   {
      object = inExpr;
      offset = inOffset;
   }
   ExprType getType()
   {
      return (ExprType) ExprTypeOf<T>::value;
   }
   CppiaExpr *link(CppiaData &inData)
   {
      if (object)
         object = object->link(inData);
      return this;
   }


   void        runVoid(CppiaCtx *ctx) { }
   int runInt(CppiaCtx *ctx) { return ValToInt( MEMGETVAL ); }
   Float       runFloat(CppiaCtx *ctx) { return ValToFloat( MEMGETVAL ); }
   ::String    runString(CppiaCtx *ctx) { return ValToString( MEMGETVAL ); }
   hx::Object *runObject(CppiaCtx *ctx) {
      return Dynamic( MEMGETVAL ).mPtr;
   }

   CppiaExpr  *makeSetter(CppiaExpr *value);
};

template<typename T, int REFMODE> 
struct MemReferenceSetter : public CppiaExpr
{
   int offset;
   CppiaExpr *object;
   CppiaExpr *value;

   MemReferenceSetter(MemReference<T,REFMODE> *inSrc, CppiaExpr *inValue) : CppiaExpr(inSrc)
   {
      offset = inSrc->offset;
      object = inSrc->object;
      value = inValue;
   }
   ExprType getType()
   {
      return (ExprType) ExprTypeOf<T>::value;
   }

   void        runVoid(CppiaCtx *ctx) { runValue( MEMGETVAL, ctx, value); }
   int runInt(CppiaCtx *ctx) { return ValToInt( runValue(MEMGETVAL,ctx, value ) ); }
   Float       runFloat(CppiaCtx *ctx) { return ValToFloat( runValue(MEMGETVAL,ctx, value) ); }
   ::String    runString(CppiaCtx *ctx) { return ValToString( runValue( MEMGETVAL,ctx, value) ); }

   hx::Object *runObject(CppiaCtx *ctx) { return Dynamic( runValue(MEMGETVAL,ctx,value) ).mPtr; }

};


template<typename T, int REFMODE> 
CppiaExpr *MemReference<T,REFMODE>::makeSetter(CppiaExpr *value)
{
   return new MemReferenceSetter<T,REFMODE>(this,value);
}

struct GetFieldByLinkage : public CppiaExpr
{
   int         fieldId;
   int         typeId;
   CppiaExpr   *object;
  
   GetFieldByLinkage(CppiaStream &stream,bool inThisObject)
   {
      typeId = stream.getInt();
      fieldId = stream.getInt();
      object = inThisObject ? 0 : createCppiaExpr(stream);
   }
   CppiaExpr *link(CppiaData &inData)
   {
      TypeData *type = inData.types[typeId];
      String field = inData.strings[fieldId];

      int offset = 0;
      CppiaExpr *replace = 0;
      FieldStorage storeType = fsUnknown;

      if (type->haxeClass.mPtr)
      {
         const StorageInfo *store = type->haxeClass.mPtr->GetMemberStorage(field);
         if (store)
         {
            offset = store->offset;
            storeType = store->type;
            //printf(" found haxe var %s = %d (%d)\n", field.__s, offset, storeType);
         }
      }

      if (!offset && type->cppiaClass)
      {
         CppiaVar *var = type->cppiaClass->findVar(false, fieldId);
         if (var)
         {
            offset = var->offset;
            storeType = var->storeType;
            //printf(" found script var %s = %d (%d)\n", field.__s, offset, storeType);
         }
      }

      if (offset)
      {
         switch(storeType)
         {
            case fsBool:
               replace = object ?
                     (CppiaExpr*)new MemReference<bool,locObj>(this,offset,object):
                     (CppiaExpr*)new MemReference<bool,locThis>(this,offset);
                  break;
            case fsInt:
               replace = object ?
                     (CppiaExpr*)new MemReference<int,locObj>(this,offset,object):
                     (CppiaExpr*)new MemReference<int,locThis>(this,offset);
                  break;
            case fsFloat:
               replace = object ?
                     (CppiaExpr*)new MemReference<Float,locObj>(this,offset,object):
                     (CppiaExpr*)new MemReference<Float,locThis>(this,offset);
                  break;
            case fsString:
               replace = object ?
                     (CppiaExpr*)new MemReference<String,locObj>(this,offset,object):
                     (CppiaExpr*)new MemReference<String,locThis>(this,offset);
                  break;
            case fsObject:
               replace = object ?
                     (CppiaExpr*)new MemReference<hx::Object *,locObj>(this,offset,object):
                     (CppiaExpr*)new MemReference<hx::Object *,locThis>(this,offset);
                  break;
            case fsByte:
            case fsUnknown:
                ;// todo
         }
      }

      if (replace)
      {
         replace = replace->link(inData);
         delete this;
         return replace;
      }

      printf("GetFieldByName %s (%p %p) -> %s fallback\n", type->name.__s, type->haxeClass.mPtr, type->cppiaClass, field.__s);
      CppiaExpr *result = new GetFieldByName(this, fileId, object);
      result = result->link(inData);
      delete this;
      return result;
   }
};




struct StringVal : public CppiaExprWithValue
{
   int stringId;
   String strVal;

   StringVal(int inId) : stringId(inId)
   {
   }

   ExprType getType() { return etString; }

   CppiaExpr *link(CppiaData &inData)
   {
      strVal = inData.strings[stringId];
      printf("Linked %d -> %s\n", stringId, strVal.__s);
      return CppiaExprWithValue::link(inData);
   }
   ::String    runString(CppiaCtx *ctx)
   {
      return strVal;
   }
   hx::Object *runObject(CppiaCtx *ctx)
   {
      if (!value.mPtr)
         value = strVal;
      return value.mPtr;
   }

   void mark(hx::MarkContext *__inCtx)
   {
      HX_MARK_MEMBER(value);
      HX_MARK_MEMBER(strVal);
   }
   void visit(hx::VisitContext *__inCtx)
   {
      HX_VISIT_MEMBER(value);
      HX_VISIT_MEMBER(strVal);
   }

};


template<typename T>
struct DataVal : public CppiaExprWithValue
{
   T data;

   DataVal(T inVal) : data(inVal)
   {
   }

   ExprType getType() { return (ExprType)ExprTypeOf<T>::value; }

   void        runVoid(CppiaCtx *ctx) {  }
   int runInt(CppiaCtx *ctx) { return ValToInt(data); }
   Float       runFloat(CppiaCtx *ctx) { return ValToFloat(data); }
   ::String    runString(CppiaCtx *ctx) { return ValToString(data); }

   hx::Object *runObject(CppiaCtx *ctx)
   {
      if (!value.mPtr)
         value = Dynamic(data);
      return value.mPtr;
   }
};



struct NullVal : public CppiaExpr
{
   NullVal() { }
   ExprType getType() { return etObject; }

   void        runVoid(CppiaCtx *ctx) {  }
   int runInt(CppiaCtx *ctx) { return 0; }
   Float       runFloat(CppiaCtx *ctx) { return 0.0; }
   ::String    runString(CppiaCtx *ctx) { return null(); }
   hx::Object  *runObject(CppiaCtx *ctx) { return 0; }

};


struct PosInfo : public CppiaExprWithValue
{
   int fileId;
   int line;
   int classId;
   int methodId;

   PosInfo(CppiaStream &stream)
   {
      fileId = stream.getInt();
      line = stream.getInt();
      classId = stream.getInt();
      methodId = stream.getInt();
   }
   CppiaExpr *link(CppiaData &inData)
   {
      String clazz = inData.strings[classId];
      String file = inData.strings[fileId];
      String method = inData.strings[methodId];
      value = hx::SourceInfo(file,line,clazz,method);
      return CppiaExprWithValue::link(inData);
   }
};

struct ArrayDef : public CppiaExpr
{
   int classId;
   Expressions items;
   ArrayType arrayType;

   ArrayDef(CppiaStream &stream)
   {
      classId = stream.getInt();
      ReadExpressions(items,stream);
   }


   CppiaExpr *link(CppiaData &inData)
   {
      TypeData *type = inData.types[classId];
      arrayType = type->arrayType;
      if (!arrayType)
      {
         printf("ArrayDef of non array-type %s\n", type->name.__s);
         throw "Bad ArrayDef";
      }
      LinkExpressions(items,inData);
      return this;
   }
   hx::Object *runObject(CppiaCtx *ctx)
   {
      int n = items.size();
      switch(arrayType)
      {
         case arrBool:
            { 
            Array<bool> result = Array_obj<bool>::__new(n,n);
            for(int i=0;i<n;i++)
               result[i] = items[i]->runInt(ctx)!=0;
            return result.mPtr;
            }
         case arrUnsignedChar:
            { 
            Array<unsigned char> result = Array_obj<unsigned char>::__new(n,n);
            for(int i=0;i<n;i++)
               result[i] = items[i]->runInt(ctx);
            return result.mPtr;
            }
         case arrInt:
            { 
            Array<int> result = Array_obj<int>::__new(n,n);
            for(int i=0;i<n;i++)
               result[i] = items[i]->runInt(ctx);
            return result.mPtr;
            }
         case arrFloat:
            { 
            Array<Float> result = Array_obj<Float>::__new(n,n);
            for(int i=0;i<n;i++)
               result[i] = items[i]->runFloat(ctx);
            return result.mPtr;
            }
         case arrString:
            { 
            Array<String> result = Array_obj<String>::__new(n,n);
            for(int i=0;i<n;i++)
               result[i] = items[i]->runString(ctx);
            return result.mPtr;
            }
         case arrDynamic:
            { 
            Array<Dynamic> result = Array_obj<Dynamic>::__new(n,n);
            for(int i=0;i<n;i++)
               result[i] = items[i]->runObject(ctx);
            return result.mPtr;
            }
         default:
            return 0;
      }
      return 0;
   }
};

struct ArrayIExpr : public CppiaExpr
{
   int       classId;
   CppiaExpr *thisExpr;
   CppiaExpr *iExpr;
   CppiaExpr *value;


   ArrayIExpr(CppiaStream &stream)
   {
      classId = stream.getInt();
      thisExpr = createCppiaExpr(stream);
      iExpr = createCppiaExpr(stream);
      value = 0;
   }

   CppiaExpr   *makeSetter(CppiaExpr *inValue)
   {
      value = inValue;
      return this;
   }


   CppiaExpr *link(CppiaData &inData)
   {
      TypeData *type = inData.types[classId];
      if (!type->arrayType)
      {
         printf("ARRAYI of non array-type %s\n", type->name.__s);
         throw "Bad ARRAYI";
      }
      Expressions val;
      val.push_back(iExpr);

      CppiaExpr *replace = 0;
      if (value)
      {
         val.push_back(value);
         replace = createArrayBuiltin(this, type->arrayType, thisExpr, HX_CSTRING("__set"), val);
      }
      else
         replace = createArrayBuiltin(this, type->arrayType, thisExpr, HX_CSTRING("__get"), val);

      replace->link(inData);
      delete this;
      return replace;
   }

};


struct VarDecl : public CppiaExpr
{
   CppiaStackVar var;
   CppiaExpr *init;

   VarDecl(CppiaStream &stream,bool inInit)
   {
      var.fromStream(stream);
      init = 0;
      if (inInit)
      {
         int t = stream.getInt();
         init = createCppiaExpr(stream);
         init->haxeTypeId = t;
      }
   }

   void runVoid(CppiaCtx *ctx)
   {
      if (init)
      {
         switch(var.expressionType)
         {
            case etInt: *(int *)(ctx->frame+var.stackPos) = init->runInt(ctx); break;
            case etFloat: *(Float *)(ctx->frame+var.stackPos) = init->runFloat(ctx); break;
            case etString: *(String *)(ctx->frame+var.stackPos) = init->runString(ctx); break;
            case etObject: *(hx::Object **)(ctx->frame+var.stackPos) = init->runObject(ctx); break;
            case etVoid:
            case etNull:
               break;
          }
      }
   }

   CppiaExpr *link(CppiaData &inData)
   {
      var.link(inData);
      init = init ? init->link(inData) : 0;
      return this;
   }
};



struct VarRef : public CppiaExpr
{
   int varId;
   CppiaStackVar *var;
   ExprType type;
   int      stackPos;

   VarRef(CppiaStream &stream)
   {
      varId = stream.getInt();
      var = 0;
      type = etVoid;
      stackPos = 0;
   }

   ExprType getType() { return type; }

   CppiaExpr *link(CppiaData &inData)
   {
      var = inData.layout->findVar(varId);
      if (!var)
      {
         printf("Could not link var %d\n", varId);
         throw "Unknown variable";
      }

      stackPos = var->stackPos;
      type = var->expressionType;

      CppiaExpr *replace = 0;
      switch(type)
      {
         case etInt:
            replace = new MemReference<int,locStack>(this,var->stackPos);
            break;
         case etFloat:
            replace = new MemReference<Float,locStack>(this,var->stackPos);
            break;
         case etString:
            replace = new MemReference<String,locStack>(this,var->stackPos);
            break;
         case etObject:
            replace = new MemReference<hx::Object *,locStack>(this,var->stackPos);
            break;
         case etVoid:
         case etNull:
            break;
      }
      if (replace)
      {
         replace->link(inData);
         delete this;
         return replace;
      }

      return this;
   }
};


struct RetVal : public CppiaExpr
{
   bool      retVal;
   CppiaExpr *value;

   RetVal(CppiaStream &stream,bool inRetVal)
   {
      retVal = inRetVal;
      if (retVal)
      {
         int t = stream.getInt();
         value = createCppiaExpr(stream);
         value->haxeTypeId = t;
      }
      else
         value = 0;
   }

   ExprType getType() { return etVoid; }

   CppiaExpr *link(CppiaData &inData)
   {
      if (value)
         value = value->link(inData);
      return this;
   }

   void runVoid(CppiaCtx *ctx)
   {
      throw value;
   }
   hx::Object *runObject(CppiaCtx *ctx) { runVoid(ctx); return 0; }
   int runInt(CppiaCtx *ctx) { runVoid(ctx); return 0; }
   Float runFloat(CppiaCtx *ctx) { runVoid(ctx); return 0; }
   String runString(CppiaCtx *ctx) { runVoid(ctx); return null(); }

};



struct BinOp : public CppiaExpr
{
   CppiaExpr *left;
   CppiaExpr *right;
   ExprType type;

   BinOp(CppiaStream &stream)
   {
      left = createCppiaExpr(stream);
      right = createCppiaExpr(stream);
      type = etFloat;
   }

   CppiaExpr *link(CppiaData &inData)
   {
      left = left->link(inData);
      right = right->link(inData);

      if (left->getType()==etInt && right->getType()==etInt)
         type = etInt;
      else
         type = etFloat;
      return this;
   }
   hx::Object *runObject(CppiaCtx *ctx)
   {
      return Dynamic(runFloat(ctx)).mPtr;
   }
};



struct OpMult : public BinOp
{
   OpMult(CppiaStream &stream) : BinOp(stream) { }

   int runInt(CppiaCtx *ctx)
   {
      int lval = left->runInt(ctx);
      return lval * right->runInt(ctx);
   }
   Float runFloat(CppiaCtx *ctx)
   {
      Float lval = left->runFloat(ctx);
      return lval * right->runFloat(ctx);
   }
};

struct StringAdd : public CppiaExpr
{
   CppiaExpr *left;
   CppiaExpr *right;

   StringAdd(const CppiaExpr *inSrc, CppiaExpr *inLeft, CppiaExpr *inRight)
      : CppiaExpr(inSrc)
   {
      left = inLeft;
      right = inRight;
   }
   String runString(CppiaCtx *ctx)
   {
      String lval = left->runString(ctx);
      return lval + right->runString(ctx);
   }
   hx::Object *runObject(CppiaCtx *ctx)
   {
      return Dynamic(runString(ctx)).mPtr;
   }
};

struct OpAdd : public BinOp
{
   OpAdd(CppiaStream &stream) : BinOp(stream)
   {
   }

   CppiaExpr *link(CppiaData &inData)
   {
      BinOp::link(inData);

      if (left->getType()==etString || right->getType()==etString)
      {
         CppiaExpr *result = new StringAdd(this,left,right);
         delete this;
         return result;
      }
      return this;
   }

   int runInt(CppiaCtx *ctx)
   {
      int lval = left->runInt(ctx);
      return lval + right->runInt(ctx);
   }
   Float runFloat(CppiaCtx *ctx)
   {
      int lval = left->runFloat(ctx);
      return lval + right->runFloat(ctx);
   }
};





CppiaExpr *createCppiaExpr(CppiaStream &stream)
{
   int fileId = stream.getInt();
   int line = stream.getInt();
   std::string tok = stream.getToken();
   printf(" expr %s\n", tok.c_str() );

   CppiaExpr *result = 0;
   if (tok=="FUN")
      result = new FunExpr(stream);
   else if (tok=="BLOCK")
      result = new BlockExpr(stream);
   else if (tok=="IFELSE")
      result = new IfElseExpr(stream);
   else if (tok=="IF")
      result = new IfExpr(stream);
   else if (tok=="ISNULL")
      result = new IsNull(stream);
   else if (tok=="NOTNULL")
      result = new IsNotNull(stream);
   else if (tok=="CALLSTATIC")
      result = new CallStatic(stream);
   else if (tok=="CALLTHIS")
      result = new CallMember(stream,true);
   else if (tok=="CALLSUPER")
      result = new CallMember(stream,true,true);
   else if (tok=="CALLMEMBER")
      result = new CallMember(stream);
   else if (tok=="true")
      result = new DataVal<bool>(true);
   else if (tok=="false")
      result = new DataVal<bool>(false);
   else if (tok[0]=='s')
      result = new StringVal(atoi(tok.c_str()+1));
   else if (tok[0]=='f')
      result = new DataVal<Float>(atof(tok.c_str()+1));
   else if (tok[0]=='i')
      result = new DataVal<int>(atoi(tok.c_str()+1));
   else if (tok=="POSINFO")
      result = new PosInfo(stream);
   else if (tok=="VAR")
      result = new VarRef(stream);
   else if (tok=="RETVAL")
      result = new RetVal(stream,true);
   else if (tok=="RETURN")
      result = new RetVal(stream,false);
   else if (tok=="CALL")
      result = new Call(stream);
   else if (tok=="FLINK")
      result = new GetFieldByLinkage(stream,false);
   else if (tok=="FTHISINST")
      result = new GetFieldByLinkage(stream,true);
   else if (tok=="FNAME")
      result = new GetFieldByName(stream,false);
   else if (tok=="FTHISNAME")
      result = new GetFieldByName(stream,true);
   else if (tok=="NULL")
      result = new NullVal();
   else if (tok=="VARDECL")
      result = new VarDecl(stream,false);
   else if (tok=="VARDECLI")
      result = new VarDecl(stream,true);
   else if (tok=="NEW")
      result = new NewExpr(stream);
   else if (tok=="ARRAYI")
      result = new ArrayIExpr(stream);
   else if (tok=="ADEF")
      result = new ArrayDef(stream);
   else if (tok=="SET")
      result = new SetExpr(stream);
   else if (tok=="+")
      result = new OpAdd(stream);
   else if (tok=="*")
      result = new OpMult(stream);

   if (!result)
      throw "invalid expression";

   result->fileId = fileId;
   result->line = line;

   return result;
}

// --- TypeData -------------------------

void TypeData::link(CppiaData &inData)
{
   if (linked)
      return;

   linked = true;

   TypeData *cppiaSuperType = 0;
   if (cppiaClass && cppiaClass->superId)
   {
     cppiaSuperType = inData.types[cppiaClass->superId];
     cppiaSuperType->link(inData);
   }

   if (name.length>0)
   {
      haxeClass = Class_obj::Resolve(name);
      if (!haxeClass.mPtr && !cppiaClass && name==HX_CSTRING("int"))
      {
         name = HX_CSTRING("Int");
         haxeClass = Class_obj::Resolve(name);
      }
      if (!haxeClass.mPtr && !cppiaClass && name==HX_CSTRING("bool"))
      {
         name = HX_CSTRING("Bool");
         haxeClass = Class_obj::Resolve(name);
      }
      if (!haxeClass.mPtr && !cppiaClass && (name==HX_CSTRING("float") || name==HX_CSTRING("double")))
      {
         name = HX_CSTRING("Float");
         haxeClass = Class_obj::Resolve(name);
      }

      printf(" link type '%s' %s ", name.__s, haxeClass.mPtr ? "native" : "script" );

      if (!haxeClass.mPtr && name.substr(0,6)==HX_CSTRING("Array."))
      {
         haxeClass = Class_obj::Resolve(HX_CSTRING("Array"));
         String t = name.substr(6,null());
         if (t==HX_CSTRING("int"))
            arrayType = arrInt;
         else if (t==HX_CSTRING("bool"))
            arrayType = arrBool;
         else if (t==HX_CSTRING("Float"))
            arrayType = arrFloat;
         else if (t==HX_CSTRING("String"))
            arrayType = arrString;
         else if (t==HX_CSTRING("unsigned char"))
            arrayType = arrUnsignedChar;
         else if (t==HX_CSTRING("Dynamic"))
            arrayType = arrDynamic;
         else
            throw "Unknown array type";

         printf("array type %d\n",arrayType);
      }
      else if (!haxeClass.mPtr && cppiaSuperType)
      {
         haxeBase = cppiaSuperType->haxeBase;
         haxeClass.mPtr = cppiaSuperType->haxeClass.mPtr;
         printf("extends %s\n", cppiaSuperType->name.__s);
      }
      else if (!haxeClass.mPtr)
      {
         if ((*sScriptRegistered)[name.__s])
            throw "New class, but with existing def";

         haxeBase = (*sScriptRegistered)["hx.Object"];
         printf("base\n");
      }
      else
      {
         haxeBase = (*sScriptRegistered)[name.__s];
         if (!haxeBase)
         {
            printf("assumed base (todo - register)\n");
            haxeBase = (*sScriptRegistered)["hx.Object"];
         }
         else
         {
            printf("\n");
         }
      }

      if (name==HX_CSTRING("Void"))
         expressionType = etVoid;
      else if (name==HX_CSTRING("Null"))
         expressionType = etNull;
      else if (name==HX_CSTRING("String"))
         expressionType = etString;
      else if (name==HX_CSTRING("Float"))
         expressionType = etFloat;
      else if (name==HX_CSTRING("Int") || name==HX_CSTRING("Bool"))
         expressionType = etInt;
      else
         expressionType = etObject;
   }
   else
   {
      haxeBase = (*sScriptRegistered)["hx.Object"];
      haxeClass = Class_obj::Resolve(HX_CSTRING("Dynamic"));
   }
}

// --- CppiaData -------------------------

CppiaData::~CppiaData()
{
   delete main;
   for(int i=0;i<classes.size();i++)
      delete classes[i];
}

void CppiaData::link()
{
   printf("Resolve registered\n");
   ScriptRegistered *hxObj = (*sScriptRegistered)["hx.Object"];
   for(ScriptRegisteredMap::iterator i = sScriptRegistered->begin();
       i!=sScriptRegistered->end();++i)
   {
      printf(" super class '%s' ", i->first.c_str());
      if (i->second == hxObj)
      {
         printf("=0\n");
         continue;
      }
      Class cls = Class_obj::Resolve( String(i->first.c_str() ) );
      if (cls.mPtr)
      {
         Class superClass = cls->GetSuper();
         if (superClass.mPtr)
         {
            ScriptRegistered *superRef = (*sScriptRegistered)[superClass.mPtr->mName.__s];
            if (superRef)
            {
               printf("registered %s\n",superClass.mPtr->mName.__s);
               i->second->haxeSuper = superRef;
            }
         }
      }

      if (!i->second->haxeSuper)
      {
         printf("using hx.Object\n");
         i->second->haxeSuper = hxObj;
      }
   }

   printf("Resolve typeIds\n");
   for(int t=0;t<types.size();t++)
      types[t]->link(*this);

   printf("Resolve inherited atributes\n");
   for(int i=0;i<classes.size();i++)
   {
      classes[i]->linkTypes();
   }

   for(int i=0;i<classes.size();i++)
      classes[i]->link();
   if (main)
      main = main->link(*this);
}

/*
CppiaClass *CppiaData::findClass(String inName)
{
   for(int i=0;i<classes.size();i++)
      if (strings[classes[i]->nameId] == inName)
         return classes[i];
   return 0;
}
*/


void CppiaData::mark(hx::MarkContext *__inCtx)
{
   HX_MARK_MEMBER(strings);
   for(int i=0;i<types.size();i++)
      types[i]->mark(__inCtx);
   for(int i=0;i<markable.size();i++)
      markable[i]->mark(__inCtx);
}

void CppiaData::visit(hx::VisitContext *__inCtx)
{
   HX_VISIT_MEMBER(strings);
   for(int i=0;i<types.size();i++)
      types[i]->visit(__inCtx);
   for(int i=0;i<markable.size();i++)
      markable[i]->visit(__inCtx);
}

// TODO  - more than one
static hx::Object *currentCppia = 0;

bool LoadCppia(String inValue)
{
   CppiaData   *cppiaPtr = new CppiaData();
   currentCppia = new CppiaObject(cppiaPtr); 
   GCAddRoot(&currentCppia);


   CppiaData   &cppia = *cppiaPtr;
   CppiaStream stream(inValue.__s, inValue.length);
   try
   {
      std::string tok = stream.getToken();
      if (tok!="CPPIA")
         throw "Bad magic";

      int stringCount = stream.getInt();
      for(int s=0;s<stringCount;s++)
         cppia.strings[s] = stream.readString();

      int typeCount = stream.getInt();
      cppia.types.resize(typeCount);
      for(int t=0;t<typeCount;t++)
         cppia.types[t] = new TypeData(stream.readString());

      int classCount = stream.getInt();
      int enumCount = stream.getInt();

      cppia.classes.resize(classCount);
      for(int c=0;c<classCount;c++)
      {
         cppia.classes[c] = new CppiaClass(cppia);
         cppia.classes[c]->load(stream);
      }

      tok = stream.getToken();
      if (tok=="MAIN")
      {
         printf("Main...\n");
         cppia.main = createCppiaExpr(stream);
      }
      else if (tok!="NOMAIN")
         throw "no main specified";

      printf("Link...\n");
      cppia.link();
      printf("--- Run --------------------------------------------\n");
      if (cppia.main)
      {
         CppiaCtx ctx;
         //cppia.main->runVoid(&ctx);
         printf("Result %s.\n", cppia.main->runString(&ctx).__s);
      }
      return true;
   } catch(const char *error)
   {
      printf("Error reading file: %s, line %d, char %d\n", error, stream.line, stream.pos);
   }
   return false;
}




// Called by haxe generated code ...
void ScriptableMark(void *inClass, hx::Object *inThis, HX_MARK_PARAMS)
{
   //HX_MARK_ARRAY(inInstanceData);
   //inHandler->markFields(inInstanceData,HX_MARK_ARG);
}

void ScriptableVisit(void *inClass, hx::Object *inThis, HX_VISIT_PARAMS)
{
   //HX_VISIT_ARRAY(inInstanceDataPtr);
   //inHandler->visitFields(*inInstanceDataPtr,HX_VISIT_ARG);
}

bool ScriptableField(hx::Object *, const ::String &,bool inCallProp,Dynamic &outResult)
{
   return false;
}

bool ScriptableField(hx::Object *, int inName,bool inCallProp,Float &outResult)
{
   return false;
}

bool ScriptableField(hx::Object *, int inName,bool inCallProp,Dynamic &outResult)
{
   return false;
}

void ScriptableGetFields(hx::Object *inObject, Array< ::String> &outFields)
{
}

bool ScriptableSetField(hx::Object *, const ::String &, Dynamic inValue,bool inCallProp, Dynamic &outValue)
{
   return false;
}


};


void __scriptable_load_cppia(String inCode)
{
   hx::LoadCppia(inCode);
}


