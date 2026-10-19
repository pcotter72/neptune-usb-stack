#ifndef NOTIFYGUI_H
#define NOTIFYGUI_H NOTIFYGUI_H

struct DevNotifyData
{
	BOOL nix;
};

struct PsdNotifyGadgets
{
    struct Node         pog_Node;         /* Linkage */
    struct PsdDevice   *pog_Device;       /* Linkage to the device used */
    ULONG              *pog_GroupObj;     /* Group object to link */
    ULONG              *pog_BodyObj;      /* Text body */
    ULONG              *pog_GadgetObj[4]; /* gadgets */
    struct timeval      pog_TimeoutTime;  /* time at which the thing destroys itself */
    BOOL                pog_ShowMe;       /* popup window? */
    BOOL                pog_WaitBinding;  /* wait for class scan finished before popping up */
};

struct PsdNotifySound
{
    struct Node         pps_Node;         /* Linkage */
    ULONG              *pps_DTHandle;     /* Handle to DataType object */
};

void pNotifyGUITaskCleanup(struct PsdBase *ps);
void pEventHandler(struct PsdBase *ps);
void pFreeNotifyGadgets(struct PsdBase *ps, struct PsdNotifyGadgets *pog);
struct PsdNotifyGadgets * pAllocNotifyGadgets(struct PsdBase *ps, STRPTR body, STRPTR *gad);
STRPTR pBindingsString(struct PsdBase *ps, struct PsdDevice *pd);
ULONG pCheckConfigurable(struct PsdBase *ps, struct PsdDevice *pd);
struct PsdNotifySound * pNotifyLoadSound(struct PsdBase *ps, STRPTR name);
BOOL pNotifyPlaySound(struct PsdBase *ps, STRPTR name);
void pNotifyFreeSound(struct PsdBase *ps, struct PsdNotifySound *pps);

#define PPF_HasBinding    0x0001
#define PPF_HasClassGUI   0x0010
#define PPF_HasClsConfig  0x0020
#define PPF_HasBindingGUI 0x0100
#define PPF_HasBindConfig 0x0200

#define TAGBASE_DevNotify (TAG_USER | 29<<16)
#define MUIM_DevNotify_RemInfo           (TAGBASE_DevNotify | 0x0010)
#define MUIM_DevNotify_ConfigureClass    (TAGBASE_DevNotify | 0x0011)
#define MUIM_DevNotify_ConfigureBinding  (TAGBASE_DevNotify | 0x0012)
#define MUIM_DevNotify_ShutUp            (TAGBASE_DevNotify | 0x0013)
#define MUIM_DevNotify_DisablePort       (TAGBASE_DevNotify | 0x0014)
#define MUIM_DevNotify_PowerCyclePort    (TAGBASE_DevNotify | 0x0015)
#define MUIM_DevNotify_Sticky            (TAGBASE_DevNotify | 0x0020)
#define MUIM_DevNotify_SavePrefs         (TAGBASE_DevNotify | 0x0021)
#define MUIM_DevNotify_About             (TAGBASE_DevNotify | 0x0030)
#define MUIM_DevNotify_OpenPrefs       (TAGBASE_DevNotify | 0x0031)

IPTR DevNotifyDispatcher(struct IClass * cl asm("a0"), Object * obj asm("a2"), Msg msg asm("a1"));

#endif // NOTIFYGUI_H
