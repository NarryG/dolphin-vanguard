// A basic test implementation of Netcore for IPC in Dolphin

#pragma warning(disable : 4564)

#include "stdafx.h"

#include "Common/SPSCQueue.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/DSP.h"
#include "Core/HW/Memmap.h"
#include "Core/PatchEngine.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/State.h"

#include <iostream>
#include <string>

#include "DolphinMemoryDomain.h"
#include "VanguardClient.h"

#include <msclr/marshal_cppstd.h>

using namespace cli;
using namespace System;
using namespace RTCV;
using namespace RTCV::NetCore;
using namespace RTCV::CorruptCore;
using namespace RTCV::Vanguard;
using namespace System::Runtime::InteropServices;
using namespace System::Threading;
using namespace System::Collections::Generic;

#using < system.dll>
#using < system.reflection.dll>
#using < system.windows.forms.dll>
using namespace System::Diagnostics;

#define SRAM_SIZE 25165824
#define ARAM_SIZE 16777216
#define EXRAM_SIZE 67108864

/*
Trace::Listeners->Add(gcnew TextWriterTraceListener(Console::Out));
Trace::AutoFlush = true;
Trace::WriteLine(filename);
*/

delegate void MessageDelegate(Object ^);
// Define this in here as it's managed and it can't be in VanguardClient.h as that's included in
// unmanaged code. Could probably move this to a header
public
ref class VanguardClient
{
public:
  static RTCV::NetCore::NetCoreReceiver ^ receiver;
  static RTCV::Vanguard::VanguardConnector ^ connector;

  void OnMessageReceived(Object ^ sender, RTCV::NetCore::NetCoreEventArgs ^ e);
  void SpecUpdated(Object ^ sender, SpecUpdateEventArgs ^ e);
  void RegisterVanguardSpec();

  void StartClient();
  void RestartClient();

  bool LoadState(String ^ filename, StashKeySavestateLocation ^ location);
  bool SaveState(String ^ filename, bool wait);

  static Mutex ^ mutex = gcnew Mutex(false, "VanguardMutex");
};

ref class ManagedGlobals
{
public:
  static VanguardClient ^ client = nullptr;
};


static PartialSpec ^
    getDefaultPartial() {
      PartialSpec ^ partial = gcnew PartialSpec("RTCSpec");

      partial->Set(VSPEC::SYSTEM, String::Empty);
      return partial;
    }

    void VanguardClient::SpecUpdated(Object ^ sender, SpecUpdateEventArgs ^ e)
{
  PartialSpec ^ partial = e->partialSpec;

  LocalNetCoreRouter::Route(NetcoreCommands::CORRUPTCORE,
                            NetcoreCommands::REMOTE_PUSHVANGUARDSPECUPDATE, partial, true);
  LocalNetCoreRouter::Route(NetcoreCommands::UI, NetcoreCommands::REMOTE_PUSHVANGUARDSPECUPDATE,
                            partial, true);
}

void VanguardClient::RegisterVanguardSpec()
{
  PartialSpec ^ emuSpecTemplate = gcnew PartialSpec("VanguardSpec");

  emuSpecTemplate->Insert(getDefaultPartial());

  AllSpec::VanguardSpec =
      gcnew FullSpec(emuSpecTemplate, true);  // You have to feed a partial spec as a template

  // if (VanguardCore.attached)
  // RTCV.Vanguard.VanguardConnector.PushVanguardSpecRef(VanguardCore.VanguardSpec);

  LocalNetCoreRouter::Route(NetcoreCommands::CORRUPTCORE, NetcoreCommands::REMOTE_PUSHVANGUARDSPEC,
                            emuSpecTemplate, true);
  LocalNetCoreRouter::Route(NetcoreCommands::UI, NetcoreCommands::REMOTE_PUSHVANGUARDSPEC,
                            emuSpecTemplate, true);
  AllSpec::VanguardSpec->SpecUpdated +=
      gcnew EventHandler<NetCore::SpecUpdateEventArgs ^>(this, &VanguardClient::SpecUpdated);
}

bool isWii()
{
  if (SConfig::GetInstance().bWii)
    return true;
  return false;
}

array<MemoryDomainProxy ^> ^
    GetInterfaces() {
      array<MemoryDomainProxy ^> ^ interfaces = gcnew array<MemoryDomainProxy ^>(2);
      interfaces[0] = (gcnew MemoryDomainProxy(gcnew SRAM));
      if (isWii())
        interfaces[1] = (gcnew MemoryDomainProxy(gcnew EXRAM));
      else
        interfaces[1] = (gcnew MemoryDomainProxy(gcnew ARAM));

      return interfaces;
    }

    bool RefreshDomains()
{
  auto interfaces = GetInterfaces();
  AllSpec::VanguardSpec->Update(VSPEC::MEMORYDOMAINS_INTERFACES, interfaces, true, true);
  LocalNetCoreRouter::Route(NetcoreCommands::CORRUPTCORE,
                            NetcoreCommands::REMOTE_EVENT_DOMAINSUPDATED, true, true);
  return true;
}

// Create our VanguardClient
void VanguardClientInitializer::Initialize()
{
  System::Windows::Forms::Form ^ dummy = gcnew System::Windows::Forms::Form();
  IntPtr Handle = dummy->Handle;
  SyncObjectSingleton::SyncObject = dummy;

  // Start everything
  ManagedGlobals::client = gcnew VanguardClient;
  ManagedGlobals::client->StartClient();
  ManagedGlobals::client->RegisterVanguardSpec();
  RTCV::CorruptCore::CorruptCore::StartEmuSide();
}

int CPU_STEP_Count = 0;
static void STEP_CORRUPT()  // errors trapped by CPU_STEP
{
  StepActions::Execute();
  CPU_STEP_Count++;
  bool autoCorrupt = RTCV::CorruptCore::CorruptCore::AutoCorrupt;
  long errorDelay = RTCV::CorruptCore::CorruptCore::ErrorDelay;
  if (autoCorrupt && CPU_STEP_Count >= errorDelay)
  {
    array<System::String ^> ^ domains =
        AllSpec::UISpec->Get<array<System::String ^> ^>("SELECTEDDOMAINS");

    BlastLayer ^ bl = RTCV::CorruptCore::CorruptCore::GenerateBlastLayer(domains);
    if (bl != nullptr)
      bl->Apply(false, true);
  }
}

void VanguardClientUnmanaged::CORE_STEP()
{
  // Any step hook for corruption
  STEP_CORRUPT();
}
void VanguardClientUnmanaged::LOAD_GAME_START()
{
  StepActions::ClearStepBlastUnits();
  CPU_STEP_Count = 0;
}

void VanguardClientUnmanaged::LOAD_GAME_DONE(std::string romPath)
{
  std::string s_current_file_name = "";

  PartialSpec ^ gameDone = gcnew PartialSpec("VanguardSpec");
  gameDone->Set(VSPEC::SYSTEM, "DOLPHIN");
  gameDone->Set(VSPEC::GAMENAME, gcnew String(SConfig::GetInstance().GetGameID().c_str()));
  gameDone->Set(VSPEC::SYSTEMPREFIX, "DOLPHIN");

  gameDone->Set(VSPEC::SYSTEMCORE, isWii() ? "WII" : "GAMECUBE");

  System::String ^ gameName = gcnew String(romPath.c_str());
  gameDone->Set(VSPEC::SYNCSETTINGS, "");
  gameDone->Set(VSPEC::OPENROMFILENAME, gameName);
  gameDone->Set(VSPEC::MEMORYDOMAINS_BLACKLISTEDDOMAINS, "");
  gameDone->Set(VSPEC::MEMORYDOMAINS_INTERFACES, GetInterfaces());
  gameDone->Set(VSPEC::CORE_DISKBASED, true);
  AllSpec::VanguardSpec->Update(gameDone, true, true);

  // This is local. If the domains changed it propgates over netcore
  LocalNetCoreRouter::Route(NetcoreCommands::CORRUPTCORE,
                            NetcoreCommands::REMOTE_EVENT_DOMAINSUPDATED, true, true);
}
// Initialize it
void VanguardClient::StartClient()
{
  VanguardClient::receiver = gcnew RTCV::NetCore::NetCoreReceiver();
  VanguardClient::receiver->MessageReceived +=
      gcnew EventHandler<NetCore::NetCoreEventArgs ^>(this, &VanguardClient::OnMessageReceived);
  VanguardClient::connector = gcnew RTCV::Vanguard::VanguardConnector(receiver);
}

void VanguardClient::RestartClient()
{
  VanguardClient::connector->Kill();
  VanguardClient::connector = nullptr;
  StartClient();
}

bool VanguardClient::SaveState(String ^ filename, bool wait)
{
  std::string converted_filename = msclr::interop::marshal_as<std::string>(filename);
  State::SaveAs(converted_filename, wait);
  return true;
}

/*ENUMS FOR THE SWITCH STATEMENT*/
enum COMMANDS
{
  SAVESAVESTATE,
  LOADSAVESTATE,
  REMOTE_LOADROM,
  REMOTE_CLOSEGAME,
  REMOTE_DOMAIN_GETDOMAINS,
  REMOTE_KEY_SETSYNCSETTINGS,
  REMOTE_KEY_SETSYSTEMCORE,
  REMOTE_EVENT_EMU_MAINFORM_CLOSE,
  REMOTE_EVENT_EMUSTARTED,
  REMOTE_ISNORMALADVANCE,
  REMOTE_EVENT_CLOSEEMULATOR,
  REMOTE_ALLSPECSSENT,
  UNKNOWN
};

inline COMMANDS CheckCommand(String ^ inString)
{
  if (inString == "LOADSAVESTATE")
    return LOADSAVESTATE;
  if (inString == "SAVESAVESTATE")
    return SAVESAVESTATE;
  if (inString == "REMOTE_LOADROM")
    return REMOTE_LOADROM;
  if (inString == "REMOTE_CLOSEGAME")
    return REMOTE_CLOSEGAME;
  if (inString == "REMOTE_ALLSPECSSENT")
    return REMOTE_DOMAIN_GETDOMAINS;
  if (inString == "REMOTE_DOMAIN_GETDOMAINS")
    return REMOTE_KEY_SETSYNCSETTINGS;
  if (inString == "REMOTE_KEY_SETSYNCSETTINGS")
    return REMOTE_KEY_SETSYSTEMCORE;
  if (inString == "REMOTE_KEY_SETSYSTEMCORE")
    return REMOTE_KEY_SETSYSTEMCORE;
  if (inString == "REMOTE_EVENT_EMU_MAINFORM_CLOSE")
    return REMOTE_EVENT_EMU_MAINFORM_CLOSE;
  if (inString == "REMOTE_EVENT_EMUSTARTED")
    return REMOTE_EVENT_EMUSTARTED;
  if (inString == "REMOTE_ISNORMALADVANCE")
    return REMOTE_ISNORMALADVANCE;
  if (inString == "REMOTE_EVENT_CLOSEEMULATOR")
    return REMOTE_EVENT_CLOSEEMULATOR;
  if (inString == "REMOTE_ALLSPECSSENT")
    return REMOTE_ALLSPECSSENT;
  return UNKNOWN;
}

/* IMPLEMENT YOUR COMMANDS HERE */
bool VanguardClient::LoadState(String ^ filename, StashKeySavestateLocation ^ location)
{
  std::string converted_filename = msclr::interop::marshal_as<std::string>(filename);
  State::LoadAs(converted_filename);
  return true;
}
/*
Action<Object ^, EventArgs ^> ^
    LOADSTATE_NET(System::Object ^ o, NetCoreEventArgs ^ e) {
      NetCoreAdvancedMessage ^ advancedMessage = (NetCoreAdvancedMessage ^) e->message;
      array<System::Object ^> ^ cmd =
          static_cast<array<System::Object ^> ^>(advancedMessage->objectValue);
      System::String ^ path = static_cast<System::String ^>(cmd[0]);
      StashKeySavestateLocation ^ location = safe_cast<StashKeySavestateLocation ^>(cmd[1]);
      e->setReturnValue(ManagedGlobals::client->LoadState(path, location));
      return nullptr;
    }*/

template <class T, class U>
Boolean isinst(U u)
{
  return dynamic_cast<T>(u) != nullptr;
}

/* THIS IS WHERE YOU HANDLE ANY RECEIVED MESSAGES */
void VanguardClient::OnMessageReceived(Object ^ sender, NetCoreEventArgs ^ e)
{
  NetCoreMessage ^ message = e->message;

  // Can't define this unless it's used as SLN is set to treat warnings as errors.
  // NetCoreSimpleMessage ^ simpleMessage = (NetCoreSimpleMessage^)message;

  NetCoreSimpleMessage ^ simpleMessage;
  if (isinst<NetCoreSimpleMessage ^>(message))
  {
    simpleMessage = static_cast<NetCoreSimpleMessage ^>(message);
  }
  NetCoreAdvancedMessage ^ advancedMessage;
  if (isinst<NetCoreAdvancedMessage ^>(message))
  {
    advancedMessage = static_cast<NetCoreAdvancedMessage ^>(message);
  }

  switch (CheckCommand(message->Type))
  {
  case LOADSAVESTATE:
  {
    if (Core::GetState() == Core::State::Running)
    {
      NetCoreAdvancedMessage ^ advancedMessage = (NetCoreAdvancedMessage ^) e->message;
      array<System::Object ^> ^ cmd =
          static_cast<array<System::Object ^> ^>(advancedMessage->objectValue);
      System::String ^ path = static_cast<System::String ^>(cmd[0]);
      StashKeySavestateLocation ^ location = safe_cast<StashKeySavestateLocation ^>(cmd[1]);
      e->setReturnValue(ManagedGlobals::client->LoadState(path, location));
      break;
    }
  }

  break;

  case SAVESAVESTATE:
  {
    System::String ^ Key = (System::String ^)(advancedMessage->objectValue);
    // Build the shortname
    System::String ^ quickSlotName = Key + ".timejump";

    // Get the prefix for the state
    System::String ^ prefix = gcnew String(SConfig::GetInstance().GetGameID().c_str());
    prefix = prefix->Substring(prefix->LastIndexOf('\\') + 1);

    // Build up our path
    System::String ^ path =
        RTCV::CorruptCore::CorruptCore::workingDir + IO::Path::DirectorySeparatorChar + "SESSION" +
        IO::Path::DirectorySeparatorChar + prefix + "." + quickSlotName + ".State";

    // If the path doesn't exist, make it
    IO::FileInfo ^ file = gcnew IO::FileInfo(path);
    if (file->Directory != nullptr && file->Directory->Exists == false)
      file->Directory->Create();

    if (Core::GetState() == Core::State::Running)
      SaveState(path, true);
    e->setReturnValue(path);
  }
  break;
  case REMOTE_ALLSPECSSENT:
  {
  }
  break;
  case REMOTE_DOMAIN_GETDOMAINS:
  {
    RefreshDomains();
  }
  break;
  case REMOTE_KEY_SETSYNCSETTINGS:
  {
  }
  break;
  case REMOTE_KEY_SETSYSTEMCORE:
  {
  }
  break;
  case REMOTE_EVENT_EMU_MAINFORM_CLOSE:
  {
    System::Environment::Exit(0);
  }
  break;
  case REMOTE_EVENT_EMUSTARTED:
  {
  }
  break;
  case REMOTE_ISNORMALADVANCE:
  {
  }
  break;
  case REMOTE_EVENT_CLOSEEMULATOR:
  {
  }
  break;

  default:
    break;
  }
}
