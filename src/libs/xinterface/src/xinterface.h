#pragma once

#include "storm_glm.h"
#include "nodes/xi_util.h"
#include "quest_file_reader/quest_file_reader.h"
#include "renderer.h"
#include "editor/editor.h"
#include "inode.h"
#include "vma.hpp"

#include <filesystem>
#ifdef __ANDROID__
#include "mobile_alchemy.hpp"
#include "mobile_interface_leaving.hpp"
#include "mobile_facts_cache.hpp"
#include "mobile_save_list.hpp"
#include "mobile_action_frame.hpp"
namespace storm::android
{
struct UiWidgetAction; // platform/android_ui.hpp
}
#include "script_state.hpp"
#endif

class CXI_WINDOW;

class XINTERFACE : public XINTERFACE_BASE
{
  public:
    static XINTERFACE *pThis;

  protected:
    storm::QuestFileReader *pQuestService;
    VXSERVICE *pPictureService;
    VSTRSERVICE *pStringService;
    VRenderer *pRenderService;

    CXI_UTILS m_UtilContainer;

    bool bActive;
    bool bDisableControl;

    // context help
    int32_t m_nInterfaceMode;
    uint32_t m_dwContHelpColor;
    int32_t m_idHelpTexture;
    FXYRECT m_frectHelpTextureUV;
    char *m_strDefHelpTextureFile;
    FXYRECT m_frectDefHelpTextureUV;

    // parameters obtained from INI file //
    float fScale;
    XYRECT GlobalScreenRect;
    uint32_t dwScreenWidth;
    uint32_t dwScreenHeight;
    int m_nColumnQuantity; // Column quantity for grid of the previouse texture
    float m_fWaveAmplitude;
    float m_fWavePhase;
    float m_fWaveSpeed;
    int32_t m_nBlendStepMax;
    int32_t m_nBlendSpeed;
    XYRECT GlobalRect;
    FXYPOINT m_fpMouseOutZoneOffset;

    // mouse data
    bool m_bShowMouse;
    int32_t lock_x, lock_y; // center position for mouse pointer
    float fXMousePos, fYMousePos;
    int32_t m_lXMouse, m_lYMouse; // mouse pointer corrector for calculate active point
    POINT MouseSize;
    int32_t m_idTex;
    XI_ONLYONETEX_VERTEX vMouse[4];
    int32_t m_lMouseSensitive;
    bool m_bMouseClick;
    bool m_bDblMouseClick;
    int m_idButton;
    CINODE *m_pMouseNode;
    VDATA *m_pMouseWeel;

    uint32_t m_dwStoreFlag_Fog;

  public:
    XINTERFACE(XINTERFACE &&) = delete;
    XINTERFACE(const XINTERFACE &) = delete;
    XINTERFACE();
    ~XINTERFACE() override;

    void SetDevice();
    bool Init() override;
    void Execute(uint32_t Delta_Time);
    void Realize(uint32_t Delta_Time);
    uint64_t ProcessMessage(MESSAGE &message) override;
    uint32_t AttributeChanged(ATTRIBUTES *patr) override;

    void ProcessStage(Stage stage, uint32_t delta) override
    {
        switch (stage)
        {
        case Stage::execute:
            Execute(delta);
            break;
        case Stage::realize:
            Realize(delta);
            break;
        }
    }

    void CreateNode(const char *sFileName, const char *sNodeType, const char *sNodeName, int32_t priority = 80);
    void SFLB_CreateNode(INIFILE *pOwnerIni, INIFILE *pUserIni, const char *sNodeType, const char *sNodeName,
                         int32_t priority);
    void DeleteNode(const char *pcNodeName);
    CINODE *NewNode(const char *pcNodType);

    void SetTooltip(const char *pcHeader, const char *pcText1, uint32_t dwTextColor1, const char *pcText2,
                    uint32_t dwTextColor2, const char *pcText3, uint32_t dwTextColor3, const char *pcText4,
                    uint32_t dwTextColor4, const char *pcPicTextureName, const char *pcPicGroupName,
                    const char *pcPicImageName, int32_t nPicWidth, int32_t nPicHeight);

    static storm::QuestFileReader *GetQuestFileReader()
    {
        return pThis->pQuestService;
    }

    static VXSERVICE *GetPictureService()
    {
        return pThis->pPictureService;
    }

    static VSTRSERVICE *GetStringService()
    {
        return pThis->pStringService;
    }

    static VRenderer *GetRenderService()
    {
        return pThis->pRenderService;
    }

    storm::QuestFileReader *QuestFileReader() override
    {
        return pQuestService;
    }

    VXSERVICE *PictureService() override
    {
        return pPictureService;
    }

    VSTRSERVICE *StringService() override
    {
        return pStringService;
    }

    VRenderer *RenderService() override
    {
        return pRenderService;
    }

    void *GetCurrentNode() override
    {
        return m_pCurNode;
    }

    FXYPOINT GetMousePoint() override
    {
        return FXYPOINT(fXMousePos + m_lXMouse, fYMousePos + m_lYMouse);
    }

    int32_t PrintIntoWindow(int32_t wl, int32_t wr, int32_t idFont, uint32_t dwFCol, uint32_t dwBCol, int32_t align, bool shadow,
                         float scale, int32_t sxs, int32_t sys, int32_t left, int32_t top, const char *str,
                         int nWidthForScaleCorrecting = -1, int nSplit = 0) override;

    CINODE *FindNode(const char *sNodeName, CINODE *findRoot) override
    {
        if (!findRoot)
            findRoot = m_pNodes;
        if (findRoot)
            return findRoot->FindNode(sNodeName);
        return nullptr;
    }

    void ShowWindow(const char *pcWindowName, bool bShow) override;
    void DisableWindow(const char *pcWindowName, bool bDisable) override;
    void AddNodeToWindow(const char *pcNodeName, const char *pcWindowName) override;
    virtual CXI_WINDOW *FindWindow(const char *pcWindowName);
    virtual bool IsWindowActive(const char *pcWindowName);

    void RegistryExitKey(const char *pcKeyName) override;

    std::vector<std::string> m_asExitKey;

    struct LocksInfo
    {
        int32_t nSaveCode;
        std::vector<CINODE *> aNode;

        LocksInfo()
        {
            nSaveCode = -1;
        }
    };

    std::vector<LocksInfo> m_aLocksArray;

    int32_t StoreNodeLocksWithOff();
    void RestoreNodeLocks(int32_t nStoreIdx);

    int32_t GetScreenWidth() const
    {
        return dwScreenWidth;
    }

    int32_t GetScreenHeight() const
    {
        return dwScreenHeight;
    }

  protected:
    // WORLD,VIEW and PROJECTION matrixes
    glm::mat4 matw, matv, matp;

    CINODE *m_pCurNode;
    CINODE *m_pNodes;
    CINODE *m_pContHelp;
    CINODE *m_pMonocracyNode;
    GIEditor *m_pEditor;
    bool m_bNotFirstPress;
    int32_t m_nPressDelay;
    int32_t m_nMaxPressDelay;

    int32_t m_nMouseLastClickTimeCur;
    int32_t m_nMouseLastClickTimeMax;

    bool m_bUse;

    // context help functions
    void GetContextHelpData();
    void ReleaseContextHelpData();
    void ShowContextHelp();

    void DrawNode(CINODE *nod, uint32_t Delta_Time, int32_t startPrior = 0, int32_t endPrior = 32000) const;
    // initialisation function
    void LoadIni();
    void LoadDialog(const char *sFileName);
    // control function
    CINODE *GetActivingNode(CINODE *findRoot);
    void DoControl();
    void RunNodeEvents();
    void MobileInterfaceGone();
    bool SetCurNode(CINODE *pNod);
    // mouse function
    void MouseMove();
    void MouseClick(bool bFirstClick);
    void MouseDeClick();
    CINODE *GetClickNode(CINODE *searchNod, int32_t xPos, int32_t yPos) const;
    void ReleaseOld();
    void ReleaseDinamicPic(const char *sPicName);
    bool SFLB_DoSaveFileData(const char *saveName, const char *saveData) const;
    bool SFLB_GetSaveFileData(const char *saveName, int32_t bufSize, char *buf);
    char *SaveFileFind(int32_t saveNum, char *buffer, size_t bufSize, int32_t &fileSize);
    bool NewSaveFileName(const char *fileName) const;
    void DeleteSaveFile(const char *fileName);
    // The autosave record of `profile` to write next (save_autosave.hpp).
    std::string NextAutosaveRecord(const std::string &profile) const;
    // node control
    void AddNodeToList(CINODE *nod, int32_t priority = 80);
    // locked function
    void SetExclusiveNode(CINODE *nod);
    void AddExclusiveNode(CINODE *nod);
    void ExitFromExclusive();
    // Game time function
    void IncrementGameTime(uint32_t dwDeltaTime);
    // Options functions
    void SaveOptionsFile(const char *fileName, ATTRIBUTES *pAttr);
    void LoadOptionsFile(std::string_view filename, ATTRIBUTES *pAttr);
    int LoadIsExist();
    uint32_t m_dwCurDeltaTime;
    int m_dwGameTimeSec;
    int m_dwGameTimeMin;
    int m_dwGameTimeHour;

    union KEYSTATE {
        int dwKeyCode;

        struct
        {
            unsigned rightButton : 1;
            unsigned leftButton : 1;
            unsigned upButton : 1;
            unsigned downButton : 1;
            unsigned enterButton : 1;
            unsigned backButton : 1;
            unsigned shiftButton : 1;
        };
    };

    KEYSTATE oldKeyState;

    struct SAVE_FIND_DATA
    {
        char *save_file_name;
        int32_t file_size;
        std::filesystem::file_time_type time;

        SAVE_FIND_DATA *next;
    };

    SAVE_FIND_DATA *m_pSaveFindRoot;
    void ReleaseSaveFindList();
    void AddFindData(std::filesystem::path filePath);
    void Sorting_FindData();
    SAVE_FIND_DATA *GetSaveDataByIndex(int n) const;

    // dinamic strings data
    struct STRING_Entity
    {
        bool bUsed;
        int fontNum;
        uint32_t dwColor;
        char *sStringName;
        int x, y;
        int eAlignment;
        float fScale;
    };

    std::vector<STRING_Entity> m_stringes;
    int m_nStringQuantity;

#ifdef __ANDROID__
    void MobileCollectWidgets(std::vector<MobileWidget> &widgets);
    storm::mobile::ActionOutcome MobileApplyAction(storm::android::UiWidgetAction &action);
    void MobileAnswer(uint64_t request, const storm::mobile::ActionOutcome &outcome, bool queuedEvents);
    void MobileFlushAnswers();
    std::vector<uint64_t> mobileHeldAnswers_; // applied actions whose script events have not run yet
    // The interface model (Realize): collected again only when an input of the last collection
    // changed (MobileModelDue), published when the collection differs from the last one.
    struct MobileModelInputs
    {
        uint64_t opening = 0;  // storm::android::mobileScreenRevision
        uint64_t script = 0;   // storm::ScriptStateGeneration: the attribute trees a collection read
        uint64_t changes = 0;  // mobileChanges_
        std::string language;
        std::optional<storm::mobile::SaveListSnapshot> saves; // save_load only
        bool operator==(const MobileModelInputs &) const = default;
    };
    bool MobileModelDue(const std::optional<storm::mobile::SaveListSnapshot> &saves);
    void MobilePublishModel(std::vector<MobileWidget> widgets, const std::optional<storm::mobile::SaveListSnapshot> &saves);
    std::optional<MobileModelInputs> mobileModelInputs_; // of the last collection
    std::vector<MobileWidget> mobileModelWidgets_;       // the last published collection
    uint64_t mobileModelOpening_ = 0;                    // ... and its opening
    uint64_t mobileModelRevision_ = 0;
    // Messages to the interface and the touch UI's actions and touches: what changes a node's state.
    uint64_t mobileChanges_ = 0;
    // An action was applied: the model goes out again even unchanged (the UI's answer to it).
    bool mobileModelResend_ = false;
    // adb shell setprop log.tag.StormUiModelCheck DEBUG (read at each opening): every frame is
    // collected, and a collection that differs although no input changed is an error.
    bool mobileModelCheck_ = false;
    bool mobileModelDue_ = false; // this frame's collection was due (MobileModelDue), not only checked
    struct
    {
        uint64_t opening = 0, frames = 0, collections = 0, published = 0;
        uint64_t resend = 0, changes = 0, script = 0, variables = 0; // collections due to each input (one may have several)
    } mobileModelStats_; // of the opening, logged at the next one
    std::string mobileModelScreen_; // of that opening
    void MobileKeepSaveProfile();
    std::string MobileNationScore(const std::string &nation) const;
    void MobileNationPickFailed(const std::string &target) const;
    std::string MobileNationFlags() const;
    void MobileCollectNations(std::vector<MobileWidget> &widgets);
    void MobileCollectHero(std::vector<MobileWidget> &widgets);
    void MobileCollectFacts(std::vector<MobileWidget> &widgets);
    storm::mobile::InterfaceLeaving mobileLeaving_;
    bool mobileLeft_ = false; // the open interface's script has ended: no further model of it (MobileCollectFacts)
    struct MobileFacts
    {
        std::vector<MobileWidget> widgets;
        bool left = false; // mobileLeft_ as of the collection
    };
    storm::mobile::FactsCache<MobileFacts> mobileFacts_;
    mutable storm::mobile::FactsCache<std::string> mobileNationFlags_; // MobileNationFlags
    std::pair<uint64_t, std::string> mobileFactsOpening_; // the opening and its screen (diagnostics)
    uint64_t mobileFactsFrames_ = 0; // interface frames of that opening
    void MobileCollectShipyard(std::vector<MobileWidget> &widgets);
    void MobileCollectTableGame(std::vector<MobileWidget> &widgets);
    void MobileCollectGoodsTransfer(std::vector<MobileWidget> &widgets);
    bool MobileApplyGoodsTransfer(const std::string &value);
    void MobileCollectSailsGerald(std::vector<MobileWidget> &widgets);
    storm::mobile::AlchemySource MobileAlchemySource(const std::vector<MobileWidget> &widgets);
    void MobileCollectAlchemy(std::vector<MobileWidget> &widgets);
    bool MobileApplyAlchemy(const std::string &node,const std::string &value);
    void MobileCollectItemsBox(std::vector<MobileWidget> &widgets);
    bool MobileApplyItemsBoxTakeAll(const std::string &value);
    bool MobileApplyColonyLoot(const std::string &value);
    int32_t m_mobileAlchemyStrings=-1;
    std::string m_mobileAlchemyLanguage;
    bool MobileApplyShipyard(const std::string &node,int index,const std::string &value);
    std::pair<uint64_t, std::vector<MobileWidget>> m_mobileShipyardSignature; // opening and widgets the generation belongs to
    uint64_t m_mobileShipyardGeneration=0;
    std::pair<uint64_t, std::vector<MobileWidget>> m_mobileNationSignature; // opening and widgets the generation belongs to
    uint64_t m_mobileNationGeneration = 0;
#endif

    // dynamic images data
    struct IMAGE_Entity
    {
        XYRECT position;
        char *sImageListName;
        char *sImageName;
        char *sPicture;
        char *sTechniqueName;
        int32_t idTexture;
        int32_t imageID;
        bool doBlind;
        uint32_t argbBlindMin;
        uint32_t argbBlindMax;
        IMAGE_Entity *next;
    };

    IMAGE_Entity *m_imgLists;

    struct EVENT_Entity
    {
        char *sEventName;
        char *sNodeName;
        int32_t nCommandIndex;
        EVENT_Entity *next;

        EVENT_Entity()
            : next(nullptr)
        {
            sEventName = nullptr;
            sNodeName = nullptr;
            nCommandIndex = 0;
        }

        ~EVENT_Entity()
        {
            delete[] sEventName;
            sEventName = nullptr;
            delete[] sNodeName;
            sNodeName = nullptr;
            nCommandIndex = 0;
        }
    };

    EVENT_Entity *m_pEvents;

    // previouse texture & draw to texturer data
    bool m_bShowPrevTexture; // exchange one interface to other
    int32_t m_ChangeType;       // exchange type
    float m_fAngle;
    int32_t m_nBlendColor;

    // vertex & index data
    int32_t vBuf, iBuf;
    uint32_t nVert, nIndx;

    // blind parameters
    float m_fBlindFactor;
    float m_fBlindSpeed;
    bool m_bBlindUp;

    CINODE *m_pGlowCursorNode;

    CINODE *m_pCurToolTipNode;
};

class CONTROLS_CONTAINER : public Entity
{
    struct CONTEINER_DESCR
    {
        float fMaxVal;
        char *resultName;

        struct CONTROL_DESCR
        {
            float fValLimit;
            char *controlName;

            CONTROL_DESCR *next;
        } * pControls;

        CONTEINER_DESCR *next;

        CONTROL_DESCR *FindControl(const char *cntrlName);
    } * pContainers;

  public:
    CONTROLS_CONTAINER();
    ~CONTROLS_CONTAINER() override;
    bool Init() override;
    void Execute(uint32_t delta_time);
    uint64_t ProcessMessage(MESSAGE &message) override;

    void ProcessStage(Stage stage, uint32_t delta) override
    {
        switch (stage)
        {
        case Stage::execute:
            Execute(delta);
            break;
        }
    }

  protected:
    bool CreateConteinerList(ATTRIBUTES *pA);
    void AddContainer(const char *container);
    void SetContainerLimitVal(const char *container, float fLimitVal);
    void AddControlsToContainer(const char *container, const char *controlName, float fValLimit);

    CONTEINER_DESCR *FindContainer(const char *sContainer);
};
