
#include "BaseUtil.h"
#include "AppPrefs.h"
#include "AppTools.h"
#include "Doc.h"
#include "FileUtil.h"
#include "Notifications.h"
#include "Selection.h"
#include "SumatraDialogs.h"
#include "SumatraPDF.h"
#include "Translations.h"
#include "UITask.h"
#include "WindowInfo.h"
#include "WinUtil.h"
#include "resource1.h"
#include <vector>


static HANDLE pan_PrintMutex;
#define PNUM 3
#define MAXPAGERANGES 50
static PRINTER_INFO_2 gPrinterInfo[PNUM];
static LPDEVMODE gpDevMode[PNUM];
static int gIsReady[PNUM];
static int gIsPrinting;
void  pan_OnPrint(WindowInfo *win);
void pan_OnprintEx(WindowInfo *win);
static WCHAR *FormatPageSize(BaseEngine *engine, int pageNo, int rotation)
{
	RectD mediabox = engine->PageMediabox(pageNo);
	SizeD size = engine->Transform(mediabox, pageNo, 1.0, rotation).Size();

	WCHAR unitSystem[2];
	GetLocaleInfo(LOCALE_USER_DEFAULT, LOCALE_IMEASURE, unitSystem, dimof(unitSystem));
	bool isMetric = unitSystem[0] == '0';
	double unitsPerInch = isMetric ? 2.54 : 1.0;

	double width = size.dx * unitsPerInch / engine->GetFileDPI();
	double height = size.dy * unitsPerInch / engine->GetFileDPI();
	if (((int)(width * 100)) % 100 == 99)
		width += 0.01;
	if (((int)(height * 100)) % 100 == 99)
		height += 0.01;

	ScopedMem<WCHAR> strWidth(str::FormatFloatWithThousandSep(width));
	ScopedMem<WCHAR> strHeight(str::FormatFloatWithThousandSep(height));

	return str::Format(L"%s x %s %s", strWidth, strHeight, isMetric ? L"cm" : L"in");
}
static WCHAR *getPageSize(BaseEngine *engine, int pageNo, int rotation)
{
	RectD mediabox = engine->PageMediabox(pageNo);
	SizeD size = engine->Transform(mediabox, pageNo, 1.0, rotation).Size();

	WCHAR unitSystem[2];
	GetLocaleInfo(LOCALE_USER_DEFAULT, LOCALE_IMEASURE, unitSystem, dimof(unitSystem));
	bool isMetric = unitSystem[0] == '0';
	double unitsPerInch = isMetric ? 2.54 : 1.0;

	double width = size.dx * unitsPerInch / engine->GetFileDPI();
	double height = size.dy * unitsPerInch / engine->GetFileDPI();
	if (((int)(width * 100)) % 100 == 99)
		width += 0.01;
	if (((int)(height * 100)) % 100 == 99)
		height += 0.01;

	ScopedMem<WCHAR> strWidth(str::FormatFloatWithThousandSep(width));
	ScopedMem<WCHAR> strHeight(str::FormatFloatWithThousandSep(height));

	return str::Format(L"%s %s", strWidth, strHeight);
}

struct DiskPrinterInfo
{
	WCHAR DriverName[200];
	WCHAR PrinterName[200];
	WCHAR PortName[200];
};
class AbortCookieManager {
	CRITICAL_SECTION cookieAccess;
public:
	AbortCookie *cookie;

	AbortCookieManager() : cookie(NULL) {
		InitializeCriticalSection(&cookieAccess);
	}
	~AbortCookieManager() {
		Clear();
		DeleteCriticalSection(&cookieAccess);
	}

	void Abort() {
		ScopedCritSec scope(&cookieAccess);
		if (cookie)
			cookie->Abort();
		Clear();
	}

	void Clear() {
		ScopedCritSec scope(&cookieAccess);
		if (cookie) {
			delete cookie;
			cookie = NULL;
		}
	}
};

struct PrintData {
	ScopedMem<WCHAR> driverName, printerName, portName;
	ScopedMem<DEVMODE> devMode;
	BaseEngine *engine;
	Vec<PRINTPAGERANGE> ranges; // empty when printing a selection
	Vec<SelectionOnPage> sel;   // empty when printing a page range
	Print_Advanced_Data advData;
	int rotation;

	PrintData(BaseEngine *engine, PRINTER_INFO_2 *printerInfo, DEVMODE *devMode,
		Vec<PRINTPAGERANGE>& ranges, Print_Advanced_Data& advData,
		int rotation=0, Vec<SelectionOnPage> *sel=NULL) :
	engine(NULL), advData(advData), rotation(rotation)
	{
		if (engine)
			this->engine = engine->Clone();

		if (printerInfo) {
			driverName.Set(str::Dup(printerInfo->pDriverName));
			printerName.Set(str::Dup(printerInfo->pPrinterName));
			portName.Set(str::Dup(printerInfo->pPortName));
		}
		if (devMode)
			this->devMode.Set((LPDEVMODE)memdup(devMode, devMode->dmSize + devMode->dmDriverExtra));

		if (!sel)
			this->ranges = ranges;
		else
			this->sel = *sel;
	}

	~PrintData() {
		delete engine;
	}
};
class pan_PageRange;

class pan_Printer
{
public:
	pan_Printer()
	{
		isReady = 0;
		printerInfo.pDriverName = NULL;
		printerInfo.pPrinterName = NULL;
		printerInfo.pPortName = NULL;
		pDevMode = NULL;
		filePath = NULL;
		printerName = L"未配置的打印机";
	}
	~pan_Printer()
	{
		free(printerInfo.pDriverName);
		free(printerInfo.pPrinterName);
		free(printerInfo.pPortName);
		free(pDevMode);
		free(filePath);
		
	}
	int loadPrinterFromPd(PRINTDLGEX & pd)
	{
		LPDEVMODE devMode = (LPDEVMODE)GlobalLock(pd.hDevMode);
		LPDEVNAMES devNames = (LPDEVNAMES)GlobalLock(pd.hDevNames);

		if (!devMode || !devNames) 
			return 0;
		
		free(this->pDevMode);
		this->pDevMode = (LPDEVMODE)memdup(devMode,devMode->dmSize + devMode->dmDriverExtra);

		free(printerInfo.pDriverName);
		free(printerInfo.pPrinterName);
		free(printerInfo.pPortName);
		printerInfo.pDriverName = str::Dup((LPWSTR)devNames + devNames->wDriverOffset);
		printerInfo.pPrinterName = str::Dup((LPWSTR)devNames + devNames->wDeviceOffset);
		printerInfo.pPortName = str::Dup((LPWSTR)devNames + devNames->wOutputOffset);
		GlobalUnlock(pd.hDevMode);
		GlobalUnlock(pd.hDevNames);
		GlobalFree(pd.hDevNames);
		GlobalFree(pd.hDevMode);
		printerName = printerInfo.pPrinterName;
		isReady = 1;
		return 1;
	}
	int loadPrinterFromFile()
	{
		DiskPrinterInfo diskPrinterInfo;
		WORD nDevMode;
		FILE *f;
		
		_wfopen_s(&f,filePath,L"rb");
		if(!f)
			return 0;
		fread(&diskPrinterInfo,sizeof(diskPrinterInfo),1,f);
		fread(&nDevMode,sizeof(nDevMode),1,f);
		free(pDevMode);
		pDevMode =(LPDEVMODE) malloc(nDevMode);
		fread(pDevMode,nDevMode,1,f);

		free(printerInfo.pDriverName);
		free(printerInfo.pPrinterName);
		free(printerInfo.pPortName);
		printerInfo.pDriverName =str::Dup(diskPrinterInfo.DriverName);
		printerInfo.pPrinterName = str::Dup(diskPrinterInfo.PrinterName);
		printerInfo.pPortName = str::Dup(diskPrinterInfo.PortName);
		printerName = printerInfo.pPrinterName;
		isReady = 1;
		fclose(f);
		return 1;
	}

	int savePrinterToFile()
	{
		FILE *f;
		DiskPrinterInfo diskPrinterInfo;
		_wfopen_s(&f,filePath,L"wb");
		if(!f)
			return 0;

		wcscpy_s(diskPrinterInfo.DriverName,printerInfo.pDriverName);
		wcscpy_s(diskPrinterInfo.PrinterName,printerInfo.pPrinterName);
		wcscpy_s(diskPrinterInfo.PortName,printerInfo.pPortName);
		fwrite(&diskPrinterInfo,sizeof(DiskPrinterInfo),1,f);

		WORD nDevMode = pDevMode->dmSize + pDevMode->dmDriverExtra;
		fwrite(&nDevMode,sizeof(nDevMode),1,f);
		fwrite(pDevMode,nDevMode,1,f);
		fclose(f);
		return 1;
	}
	void setFilePath(WCHAR * path)  //使用内存转移
	{
		free(filePath);
		filePath = path;
	}
	void reset()
	{
		free(printerInfo.pDriverName);
		free(printerInfo.pPrinterName);
		free(printerInfo.pPortName);
		free(pDevMode);
		free(filePath);
		printerInfo.pDriverName = NULL;
		printerInfo.pPrinterName = NULL;
		printerInfo.pPortName = NULL;
		pDevMode = NULL;
		filePath = NULL;
		isReady = 0;
		printerName = L"未配置的打印机";
	}

	PRINTER_INFO_2 printerInfo;
	LPDEVMODE pDevMode;

	
	int isReady;
	WCHAR * filePath;
	WCHAR * printerName;
	pan_PageRange *pageRange;

};



class pan_PageRange   //看作一个集合，看作属性，两者是不同的东西。   页码从1开始
{
	char page[1000];
	DisplayModel * dm;
public:
	Vec<PRINTPAGERANGE> ppr;
	wchar_t * rangeName;
	pan_PageRange(DisplayModel *dm)
	{
		memset(page,0,sizeof(page));
		this->dm = dm;
	}
	int isEmpty()
	{
		int i;
		for (i = 0; i< 1000; i++)
		{
			if(page[i])
			{
				return 0;
			}
		}
		return 1;
	}
	void clear()
	{
		memset(page,0,sizeof(page));
	}
	void set(int p,int val)     
	{
		if(val == 1)
			page[p>>3] |=  1 << (7&p);//|或运算  不能实现设为0
		else if(val == 0)
			page[p>>3] &=  ~(1<<(7&p));  //设0 要 &与运算
	}
	int get(int p)
	{
		return page[p>>3] & 1<<(7&p);
	}
	void format()          //需要先清零再将char page[1000] 格式化到 Vec<PRINTPAGERANGE> ppr
	{
		int i;
		int begin,end;
		int b;
		PRINTPAGERANGE pr;
		b = 1;
		begin = 0;
		ppr.Reset();
		for (i=1;i<= dm->PageCount();i++)//页码从1开始
		{
			if(get(i) && b)
			{
				begin = i;
				b = 0;
			}
			if(!get(i) && !b)
			{
				end = i -1;
				b = 1;
				pr.nFromPage = begin;
				pr.nToPage = end;
				ppr.Append(pr);
			}
		}
		if(get(i-1))
		{
			pr.nFromPage = begin;
			pr.nToPage = i - 1;
			ppr.Append(pr);
		}
	}
};

class PageSize
{
public:
	double a;
	double b;
	wchar_t * sizeName;
};
class pan_PageInfo
{
public:
	int type;
	wchar_t * sizeName;
};
class pan_PrintContext        //元素之间的关系太复杂，用一个Context来管理
{
	WindowInfo * win;
	DisplayModel *dm;
	HWND hDlg;
public:
	Vec<pan_PageRange*> pageRanges;
	Vec<PageSize> pageSizes;
	Vec<pan_Printer*> printers;
	std::vector<pan_PageInfo> pageInfos;  //Vec不支持resize，用vector， 1 based
	int isInit;
	int isPrinting;
	pan_PrintContext()
	{
		win = NULL;
		dm = NULL;
		isPrinting = 0;
		isInit = 0;
	}
	void init(WindowInfo * win,HWND hDlg)    //不清零的初始化只能初始化一次
	{
		if(isInit == 1)
			return ;
		isInit = 1;
		this->win = win;
		this->dm = win->dm;
		this->hDlg = hDlg;
		unsigned int i;
		pan_Printer *printer;
		PageSize ps;

		ps.a = 0;
		ps.b = 0;
		ps.sizeName = L"杂";
		pageSizes.Append(ps);
		ps.a = 29.7;
		ps.b = 42;
		ps.sizeName = L"A3";
		pageSizes.Append(ps);
		ps.a = 21;
		ps.b = 29.7;
		ps.sizeName = L"A4";
		pageSizes.Append(ps);
		ps.a = 14.8;
		ps.b = 21;
		ps.sizeName = L"A5";
		pageSizes.Append(ps);
		pan_PageRange *pageRange;
		for (i = 0; i < pageSizes.Size() ;i++)
		{
			pageRange = new pan_PageRange(dm);
			pageRange->rangeName = pageSizes[i].sizeName;
			pageRanges.Append(pageRange);
		}

		for (i = 0;i<pageSizes.Size();i++)
		{
			printer = new pan_Printer();
			printers.Append(printer);
		}
		for (i = 0;i < printers.Size(); i++)
		{
			printers[i]->setFilePath(str::Format(L"E:\\printer%d",i));
			printers[i]->loadPrinterFromFile();
		}
		
		pageInfos.resize(dm->PageCount() + 1 );
		
		generatePageRange();  
		matchRangeToPrinter();
		
	}
	~pan_PrintContext()
	{
		unsigned int i;
		for (i = 0;i<pageRanges.Size();i++)
		{
			delete pageRanges[i];
		}
		for (i = 0;i<printers.Size();i++)
		{
			delete printers[i];
		}
	}
	void addPageSize(PageSize ps)
	{
		pageSizes.Append(ps);
		pageRanges.Append(new pan_PageRange(dm));
		printers.Append(new pan_Printer());
	}
	
	int getPagePrinterNum(int page)
	{
		unsigned int j;
		for (j = 0; j< pageRanges.Size(); j++)    //实体设计导致O(n)
		{
			if (pageRanges[j]->get(page))
				break;
		}
		if(j< pageRanges.Size())
			return j;
		else
			return -1;
	}

	void setPagePrinterNum(int printer,int page)
	{
		unsigned int i;
		for (i=0;i<pageRanges.Size();i++)
		{
			pageRanges[i]->set(page,0);
		}
		pageRanges[printer]->set(page,1);

	}

private:
	int pageType(int num)   //range 函数依赖 type
	{
		WCHAR * sizeStr;
		double a,b;
		double jd = 2;
		unsigned int i;
		sizeStr = getPageSize(dm->engine,num,dm->Rotation());
		swscanf_s(sizeStr,L"%lf%lf",&a,&b,wcslen(sizeStr));
		free(sizeStr);

		for (i=1;i<pageSizes.Size();i++)
		{
			if(abs(a - pageSizes[i].a) < jd && abs(b - pageSizes[i].b) < jd)
				return i;
		}
		return 0;
	}
	void generatePageRange()
	{
		unsigned int i;
		int j;
		for (i = 0;i < pageRanges.Size();i++)
		{
			pageRanges[i]->clear();
		}
		for (i = 1;i <= unsigned int (dm->PageCount()); i++)
		{
			j = pageType(i);
			pageRanges[j]->set(i,1);
			pageInfos[i].type = j;
			pageInfos[i].sizeName = pageSizes[j].sizeName;
		}

	}
	void matchRangeToPrinter()   //range 与 printer有相同的idx，容易关联
	{
		unsigned int i;
		for (i = 0; i < pageRanges.Size(); i++)
		{
			printers[i]->pageRange = pageRanges[i];
		}
	}
};



static pan_PrintContext *printContext;


class ScopeHDC {
	HDC hdc;
public:
	ScopeHDC(HDC hdc) : hdc(hdc) { }
	~ScopeHDC() { DeleteDC(hdc); }
	operator HDC() const { return hdc; }
};

void static AbortPrinting(WindowInfo *win)
{
	if (win->printThread) {
		win->printCanceled = true;
		WaitForSingleObject(win->printThread, INFINITE);
	}
	win->printCanceled = false;
}
static RectD BoundSelectionOnPage(const Vec<SelectionOnPage>& sel, int pageNo)
{
	RectD bounds;
	for (size_t i = 0; i < sel.Count(); i++) {
		if (sel.At(i).pageNo == pageNo)
			bounds = bounds.Union(sel.At(i).rect);
	}
	return bounds;
}
class PrintThreadUpdateTask : public UITask {
	NotificationWnd *wnd;
	int current, total;
	WindowInfo *win;

public:
	PrintThreadUpdateTask(WindowInfo *win, NotificationWnd *wnd, int current, int total)
		: win(win), wnd(wnd), current(current), total(total) { }

	virtual void Execute() {
		if (WindowInfoStillValid(win) && win->notifications->Contains(wnd))
			wnd->UpdateProgress(current, total);
	}
};
static bool PrintToDevice(const PrintData& pd, ProgressUpdateUI *progressUI=NULL, AbortCookieManager *abortCookie=NULL)
{
	AssertCrash(pd.engine);
	if (!pd.engine)
		return false;
	AssertCrash(pd.printerName);
	if (!pd.printerName)
		return false;

	BaseEngine& engine = *pd.engine;
	ScopedMem<WCHAR> fileName;

	DOCINFO di = { 0 };
	di.cbSize = sizeof (DOCINFO);
	if (gPluginMode) {
		fileName.Set(ExtractFilenameFromURL(gPluginURL));
		// fall back to a generic "filename" instead of the more confusing temporary filename
		di.lpszDocName = fileName ? fileName : L"filename";
	}
	else
		di.lpszDocName = engine.FileName();

	int current = 1, total = 0;
	if (pd.sel.Count() == 0) {
		for (size_t i = 0; i < pd.ranges.Count(); i++) {
			if (pd.ranges.At(i).nToPage < pd.ranges.At(i).nFromPage)
				total += pd.ranges.At(i).nFromPage - pd.ranges.At(i).nToPage + 1;
			else
				total += pd.ranges.At(i).nToPage - pd.ranges.At(i).nFromPage + 1;
		}
	}
	else {
		for (int pageNo = 1; pageNo <= engine.PageCount(); pageNo++) {
			if (!BoundSelectionOnPage(pd.sel, pageNo).IsEmpty())
				total++;
		}
	}
	AssertCrash(total > 0);
	if (0 == total)
		return false;
	if (progressUI)
		progressUI->UpdateProgress(current, total);

	// cf. http://blogs.msdn.com/b/oldnewthing/archive/2012/11/09/10367057.aspx
	ScopeHDC hdc(CreateDC(pd.driverName, pd.printerName, pd.portName, pd.devMode));
	if (!hdc)
		return false;

	if (StartDoc(hdc, &di) <= 0)
		return false;

	// MM_TEXT: Each logical unit is mapped to one device pixel.
	// Positive x is to the right; positive y is down.
	SetMapMode(hdc, MM_TEXT);

	const SizeI paperSize(GetDeviceCaps(hdc, PHYSICALWIDTH),
		GetDeviceCaps(hdc, PHYSICALHEIGHT));
	const RectI printable(GetDeviceCaps(hdc, PHYSICALOFFSETX),
		GetDeviceCaps(hdc, PHYSICALOFFSETY),
		GetDeviceCaps(hdc, HORZRES), GetDeviceCaps(hdc, VERTRES));
	const float dpiFactor = min(GetDeviceCaps(hdc, LOGPIXELSX) / engine.GetFileDPI(),
		GetDeviceCaps(hdc, LOGPIXELSY) / engine.GetFileDPI());
	bool bPrintPortrait = paperSize.dx < paperSize.dy;
	if (pd.devMode && (pd.devMode.Get()->dmFields & DM_ORIENTATION))
		bPrintPortrait = DMORIENT_PORTRAIT == pd.devMode.Get()->dmOrientation;

	if (pd.sel.Count() > 0) {
		for (int pageNo = 1; pageNo <= engine.PageCount(); pageNo++) {
			RectD bounds = BoundSelectionOnPage(pd.sel, pageNo);
			if (bounds.IsEmpty())
				continue;

			if (progressUI)
				progressUI->UpdateProgress(current, total);

			StartPage(hdc);

			geomutil::SizeT<float> bSize = bounds.Size().Convert<float>();
			float zoom = min((float)printable.dx / bSize.dx,
				(float)printable.dy / bSize.dy);
			// use the correct zoom values, if the page fits otherwise
			// and the user didn't ask for anything else (default setting)
			if (PrintScaleShrink == pd.advData.scale)
				zoom = min(dpiFactor, zoom);
			else if (PrintScaleNone == pd.advData.scale)
				zoom = dpiFactor;

			for (size_t i = 0; i < pd.sel.Count(); i++) {
				if (pd.sel.At(i).pageNo != pageNo)
					continue;

				RectD *clipRegion = &pd.sel.At(i).rect;
				PointI offset((int)((clipRegion->x - bounds.x) * zoom), (int)((clipRegion->y - bounds.y) * zoom));
				if (pd.advData.scale != PrintScaleNone) {
					// center the selection on the physical paper
					offset.x += (int)(printable.dx - bSize.dx * zoom) / 2;
					offset.y += (int)(printable.dy - bSize.dy * zoom) / 2;
				}

				bool ok = false;
				if (!pd.advData.asImage) {
					RectI rc(offset.x, offset.y, (int)(clipRegion->dx * zoom), (int)(clipRegion->dy * zoom));
					ok = engine.RenderPage(hdc, rc, pd.sel.At(i).pageNo, zoom, pd.rotation, clipRegion, Target_Print, abortCookie ? &abortCookie->cookie : NULL);
					if (abortCookie)
						abortCookie->Clear();
				}
				else {
					short shrink = 1;
					do {
						RenderedBitmap *bmp = engine.RenderBitmap(pd.sel.At(i).pageNo, zoom / shrink, pd.rotation, clipRegion, Target_Print, abortCookie ? &abortCookie->cookie : NULL);
						if (abortCookie)
							abortCookie->Clear();
						if (bmp && bmp->GetBitmap()) {
							RectI rc(offset.x, offset.y, bmp->Size().dx * shrink, bmp->Size().dy * shrink);
							ok = bmp->StretchDIBits(hdc, rc);
						}
						delete bmp;
						shrink *= 2;
					} while (!ok && shrink < 32 && !(progressUI && progressUI->WasCanceled()));
				}
			}
			// TODO: abort if !ok?

			if (EndPage(hdc) <= 0 || progressUI && progressUI->WasCanceled()) {
				AbortDoc(hdc);
				return false;
			}
			current++;
		}

		EndDoc(hdc);
		return false;
	}

	// print all the pages the user requested
	for (size_t i = 0; i < pd.ranges.Count(); i++) {
		int dir = pd.ranges.At(i).nFromPage > pd.ranges.At(i).nToPage ? -1 : 1;
		for (DWORD pageNo = pd.ranges.At(i).nFromPage; pageNo != pd.ranges.At(i).nToPage + dir; pageNo += dir) {
			if ((PrintRangeEven == pd.advData.range && pageNo % 2 != 0) ||
				(PrintRangeOdd == pd.advData.range && pageNo % 2 == 0))
				continue;
			if (progressUI)
				progressUI->UpdateProgress(current, total);

			StartPage(hdc);

			geomutil::SizeT<float> pSize = engine.PageMediabox(pageNo).Size().Convert<float>();
			int rotation = 0;
			// Turn the document by 90 deg if it isn't in portrait mode
			if (pSize.dx > pSize.dy) {
				rotation += 90;
				Swap(pSize.dx, pSize.dy);
			}
			// make sure not to print upside-down
			rotation = (rotation % 180) == 0 ? 0 : 270;
			// finally turn the page by (another) 90 deg in landscape mode
			if (!bPrintPortrait) {
				rotation = (rotation + 90) % 360;
				Swap(pSize.dx, pSize.dy);
			}

			// dpiFactor means no physical zoom
			float zoom = dpiFactor;
			// offset of the top-left corner of the page from the printable area
			// (negative values move the page into the left/top margins, etc.);
			// offset adjustments are needed because the GDI coordinate system
			// starts at the corner of the printable area and we rather want to
			// center the page on the physical paper (except for PrintScaleNone
			// where the page starts at the very top left of the physical paper so
			// that printing forms/labels of varying size remains reliably possible)
			PointI offset(-printable.x, -printable.y);

			if (pd.advData.scale != PrintScaleNone) {
				// make sure to fit all content into the printable area when scaling
				// and the whole document page on the physical paper
				RectD rect = engine.PageContentBox(pageNo, Target_Print);
				geomutil::RectT<float> cbox = engine.Transform(rect, pageNo, 1.0, rotation).Convert<float>();
				zoom = min((float)printable.dx / cbox.dx,
					min((float)printable.dy / cbox.dy,
					min((float)paperSize.dx / pSize.dx,
					(float)paperSize.dy / pSize.dy)));
				// use the correct zoom values, if the page fits otherwise
				// and the user didn't ask for anything else (default setting)
				if (PrintScaleShrink == pd.advData.scale && dpiFactor < zoom)
					zoom = dpiFactor;
				// center the page on the physical paper
				offset.x += (int)(paperSize.dx - pSize.dx * zoom) / 2;
				offset.y += (int)(paperSize.dy - pSize.dy * zoom) / 2;
				// make sure that no content lies in the non-printable paper margins
				geomutil::RectT<float> onPaper(printable.x + offset.x + cbox.x * zoom,
					printable.y + offset.y + cbox.y * zoom,
					cbox.dx * zoom, cbox.dy * zoom);
				if (onPaper.x < printable.x)
					offset.x += (int)(printable.x - onPaper.x);
				else if (onPaper.BR().x > printable.BR().x)
					offset.x -= (int)(onPaper.BR().x - printable.BR().x);
				if (onPaper.y < printable.y)
					offset.y += (int)(printable.y - onPaper.y);
				else if (onPaper.BR().y > printable.BR().y)
					offset.y -= (int)(onPaper.BR().y - printable.BR().y);
			}

			bool ok = false;
			if (!pd.advData.asImage) {
				RectI rc = RectI::FromXY(offset.x, offset.y, paperSize.dx, paperSize.dy);
				ok = engine.RenderPage(hdc, rc, pageNo, zoom, rotation, NULL, Target_Print, abortCookie ? &abortCookie->cookie : NULL);
				if (abortCookie)
					abortCookie->Clear();
			}
			else {
				short shrink = 1;
				do {
					RenderedBitmap *bmp = engine.RenderBitmap(pageNo, zoom / shrink, rotation, NULL, Target_Print, abortCookie ? &abortCookie->cookie : NULL);
					if (abortCookie)
						abortCookie->Clear();
					if (bmp && bmp->GetBitmap()) {
						RectI rc(offset.x, offset.y, bmp->Size().dx * shrink, bmp->Size().dy * shrink);
						ok = bmp->StretchDIBits(hdc, rc);
					}
					delete bmp;
					shrink *= 2;
				} while (!ok && shrink < 32 && !(progressUI && progressUI->WasCanceled()));
			}
			// TODO: abort if !ok?

			if (EndPage(hdc) <= 0 || progressUI && progressUI->WasCanceled()) {
				AbortDoc(hdc);
				return false;
			}
			current++;
		}
	}

	EndDoc(hdc);
	return true;
}

class pan_PrintThreadData : public UITask {
	//NotificationWnd *wnd;
	//AbortCookieManager cookie;
	//bool isCanceled;
	WindowInfo *win;

public:
	PrintData *data;
	HANDLE thread; // close the print thread handle after execution

	pan_PrintThreadData(WindowInfo *win, PrintData *data) :
	win(win), data(data),  thread(NULL) {
		//wnd = NULL;
		//wnd = new NotificationWnd(win->hwndCanvas, L"", _TR("Printing page %d of %d..."), this);
		//win->notifications->Add(wnd);
	}

	~pan_PrintThreadData() {
		CloseHandle(thread);
		delete data;
		//RemoveNotification(wnd);
	}

	/*virtual void UpdateProgress(int current, int total) {
		uitask::Post(new PrintThreadUpdateTask(win, wnd, current, total));
	}*/

	/*virtual bool WasCanceled() {
		return isCanceled || !WindowInfoStillValid(win) || win->printCanceled;
	}*/

	// called when printing has been canceled
	//virtual void RemoveNotification(NotificationWnd *wnd) {
	//isCanceled = true;
	//cookie.Abort();
	//this->wnd = NULL;
	////if (WindowInfoStillValid(win))
	//	//win->notifications->RemoveNotification(wnd);
	//}

	static DWORD WINAPI PrintThread(LPVOID data)    
	{
		pan_PrintThreadData *threadData = (pan_PrintThreadData *)data;
		// wait for PrintToDeviceOnThread to return so that we
		// close the correct handle to the current printing thread
		while (!threadData->win->printThread)
			Sleep(1);
		threadData->thread = threadData->win->printThread;
		PrintToDevice(*threadData->data);
		//Sleep(3000);
		//PrintToDevice(*threadData->data, threadData, &threadData->cookie);
		
		uitask::Post(threadData);
		return 0;
	}

	virtual void Execute() {
		
		if (!WindowInfoStillValid(win))
			return;
		if (win->printThread != thread)
			return;
		win->printThread = NULL;
		ReleaseSemaphore(pan_PrintMutex,1,NULL);
	}
};

static void PrintToDeviceOnThread(WindowInfo *win, PrintData *data)
{
	assert(!win->printThread);
	pan_PrintThreadData *threadData = new pan_PrintThreadData(win, data);
	win->printThread = NULL;

	win->printThread = CreateThread(NULL, 0, pan_PrintThreadData::PrintThread, threadData, 0, NULL);

}

class PrinttingControlThreadData
{
public:
	WindowInfo *win;
	PrintData **data;
	int len;
	~PrinttingControlThreadData()
	{
		delete [] data;
		
	}
};
static DWORD WINAPI PrinttingControlThread(LPVOID inData)
{
	gIsPrinting = 1;
	PrinttingControlThreadData *treadData = (PrinttingControlThreadData*) inData;
	WindowInfo *win = treadData->win;
	PrintData **data = treadData->data;
	int len = treadData->len;
	pan_PrintMutex = CreateSemaphore(NULL,1,1,NULL);
	int i;
	for (i = 0;i<len;i++)
	{
		WaitForSingleObject(pan_PrintMutex,INFINITE);
		if(data[i] ==NULL)
		{
			ReleaseSemaphore(pan_PrintMutex,1,NULL);
			continue;
		}

		PrintToDeviceOnThread(win,data[i]);   //有CreateWindowsEx函数，调用了很多主线程的wndproc，该函数不能让主线程外的函数调用
	}
	WaitForSingleObject(pan_PrintMutex,INFINITE);
	delete inData;              
	ReleaseSemaphore(pan_PrintMutex,1,NULL);
	CloseHandle(pan_PrintMutex);
	gIsPrinting = 0;
	return 0;
}



class RangesContext
{
	WindowInfo *win;
	DisplayModel *dm;
	HWND hDlg;
public:
	PRINTPAGERANGE ranges[6][MAXPAGERANGES];
	int rlen[6];
	RangesContext()
	{
		win = NULL;
		dm = NULL;
	}
	void init(HWND hDlg,WindowInfo *win)
	{
		int i;
		for (i =0;i<6;i++)
		{
			rlen[i] = 0;
		}
		this->win = win;
		this->dm = win->dm;
		this->hDlg = hDlg;
	}
	RangesContext(HWND hDlg,WindowInfo *win)
	{
		init(hDlg,win);
	}
	PRINTPAGERANGE * getRanges(int i,int &len)
	{
		len = rlen[i];
		return ranges[i];
	}

	void Classify()
	{
		int i;
		WCHAR* str;
		double a,b;
		double jd = 2;
		int pageCount = dm->PageCount();
		WCHAR * cla = new WCHAR[pageCount+1];
		int len =0;
		int curr;
		for (i = 1;i <= pageCount;i++ )
		{
			 str = FormatPageSize(dm->engine, i, dm->Rotation());
			 if(!str)
				 continue;
			
			swscanf_s(str,L"%lf %*s %lf",&a,&b,wcslen(str));
			free(str);
			if(abs(a-21) < jd && abs(b-29.7) < jd)      //A4
			{
					cla[i] = 4;
			}
			else
			if(abs(a-14.8) < jd && abs(b-21) < jd)     //A5
			{
				cla[i] = 5;
			}
			else
			if(abs(a-29.7) < jd && abs(b-42) < jd)    //A3
			{
				cla[i] = 3;
			}
			else                                //其他
			{
				cla[i] = 0;
			}
		}
		curr = cla[1];
		ranges[curr][rlen[curr]].nFromPage = 1;
		for (i=2;i<=pageCount;i++)
		{
			if(curr != cla[i])
			{
				ranges[curr][rlen[curr]++].nToPage = i-1;
				curr = cla[i];
				ranges[curr][rlen[curr]].nFromPage = i;
			}
		}
		ranges[curr][rlen[curr]++].nToPage = i-1;
		delete [] cla;
	}
	void Show()
	{
		int i;
		WCHAR text[200] = {0};
		WCHAR temp1[100] = {0};
		WCHAR temp2[100] = {0};
		int j;

		j = 3;
		for (i=0;i<rlen[j];i++)
		{
			swprintf_s(temp1,L"%d",ranges[j][i].nFromPage);
			swprintf_s(temp2,L"%d",ranges[j][i].nToPage);
			wcscat_s(text,200,temp1);
			wcscat_s(text,200,L" - ");
			wcscat_s(text,200,temp2);
			wcscat_s(text,200,L"\r\n");
		}
		
		SendDlgItemMessage(hDlg,IDC_I3,WM_SETTEXT,NULL,(LPARAM)&text);
		

		j = 4;
		memset(text,0,sizeof(text));
		for (i=0;i<rlen[j];i++)
		{
			swprintf_s(temp1,L"%d",ranges[j][i].nFromPage);
			swprintf_s(temp2,L"%d",ranges[j][i].nToPage);
			wcscat_s(text,200,temp1);
			wcscat_s(text,200,L" - ");
			wcscat_s(text,200,temp2);
			wcscat_s(text,200,L"\r\n");
		}
		
		SendDlgItemMessage(hDlg,IDC_I4,WM_SETTEXT,NULL,(LPARAM)&text);
		
		j = 0;
		memset(text,0,sizeof(text));
		for (i=0;i<rlen[j];i++)
		{
			swprintf_s(temp1,L"%d",ranges[j][i].nFromPage);
			swprintf_s(temp2,L"%d",ranges[j][i].nToPage);
			wcscat_s(text,200,temp1);
			wcscat_s(text,200,L" - ");
			wcscat_s(text,200,temp2);
			wcscat_s(text,200,L"\r\n");
		}
		
		SendDlgItemMessage(hDlg,IDC_I5,WM_SETTEXT,NULL,(LPARAM)&text);
	}
};
static RangesContext gRangesc;
PrintData * CreatePrintData(PRINTER_INFO_2& printerInfo,LPDEVMODE &pDevMode ,PRINTPAGERANGE *pRange,int nRange,WindowInfo *win)
{
	DisplayModel *dm = win->dm;
	Vec<PRINTPAGERANGE> ranges;
	int i;
	Print_Advanced_Data advanced(PrintRangeAll, PrintScaleShrink, false);
	for (i=0;i<nRange;i++)
	{
		ranges.Append(pRange[i]);
	}
	
	PrintData *data = new PrintData(dm->engine, &printerInfo, pDevMode, ranges, advanced,
		dm->Rotation(), NULL);

	free(printerInfo.pPrinterName);
	free(printerInfo.pDriverName);
	free(printerInfo.pPortName);
	free(pDevMode);
	printerInfo.pPrinterName = NULL;
	printerInfo.pDriverName = NULL;
	printerInfo.pPortName = NULL;
	pDevMode = NULL;
	return data;
}
static int GetPdFromUser(PRINTDLGEX& pd,WindowInfo* win)
{
    ZeroMemory(&pd, sizeof(PRINTDLGEX));
    pd.lStructSize = sizeof(PRINTDLGEX);
    pd.hwndOwner   = win->hwndFrame;
    pd.Flags       = PD_USEDEVMODECOPIESANDCOLLATE | PD_COLLATE;

    pd.Flags |= PD_NOSELECTION;


    pd.nCopies     = 1;
    pd.nPageRanges = 1;
    pd.nMaxPageRanges = MAXPAGERANGES;

    pd.nMinPage = 1;
    pd.nMaxPage = win->dm->PageCount();
    pd.nStartPage = START_PAGE_GENERAL;


	PRINTPAGERANGE *ppr = AllocArray<PRINTPAGERANGE>(MAXPAGERANGES);
	pd.lpPageRanges = ppr;
	ppr->nFromPage = 1;
	ppr->nToPage = win->dm->PageCount();

    if (PrintDlgEx(&pd) != S_OK) {
        if (CommDlgExtendedError() != 0) {
            /* if PrintDlg was cancelled then
               CommDlgExtendedError is zero, otherwise it returns the
               error code, which we could look at here if we wanted.
               for now just warn the user that printing has stopped
               becasue of an error */
            MessageBoxWarning(win->hwndFrame, _TR("Couldn't initialize printer"), 
                              _TR("Printing problem."));
        }
		free(ppr);

		return 0;
    }
	free(ppr);
	if (pd.dwResultAction == PD_RESULT_PRINT || pd.dwResultAction == PD_RESULT_APPLY)
		return 1;
	return 0;
}
static void TransformAndSavePd(int type,PRINTDLGEX& pd,HWND hwnd)   //功能可细分
{

	PRINTER_INFO_2 printerInfo = { 0 };
	DiskPrinterInfo diskP;
	LPDEVMODE devMode = (LPDEVMODE)GlobalLock(pd.hDevMode);
	LPDEVNAMES devNames = (LPDEVNAMES)GlobalLock(pd.hDevNames);
	if (!devMode) {
		return;
	}
	if (devNames) {
		printerInfo.pDriverName = (LPWSTR)devNames + devNames->wDriverOffset;
		printerInfo.pPrinterName = (LPWSTR)devNames + devNames->wDeviceOffset;
		printerInfo.pPortName = (LPWSTR)devNames + devNames->wOutputOffset;
		wcscpy_s(diskP.DriverName,printerInfo.pDriverName);
		wcscpy_s(diskP.PrinterName,printerInfo.pPrinterName);
		wcscpy_s(diskP.PortName,printerInfo.pPortName);
	}


	FILE *f;
	char filePath[50] = "e:\\Printer";
	filePath[strlen(filePath)] = (char)type+'0';
	filePath[strlen(filePath)+1] = 0;
	fopen_s(&f,filePath,"wb");

	WORD nDevMode = devMode->dmSize + devMode->dmDriverExtra;
	fwrite(&diskP,sizeof(DiskPrinterInfo),1,f);
	fwrite(&nDevMode,sizeof(nDevMode),1,f);
	fwrite(devMode,devMode->dmSize + devMode->dmDriverExtra,1,f);

	GlobalUnlock(pd.hDevMode);
	GlobalUnlock(pd.hDevNames);
	GlobalFree(pd.hDevNames);
	GlobalFree(pd.hDevMode);
	fclose(f);
	MessageBoxW(hwnd,L"打印机设置成功",L"提示",0);
}
int GetPrinterInfo(int type,PRINTER_INFO_2& printerInfo,LPDEVMODE &pDevMode)
{
	DiskPrinterInfo diskP;
	WORD nDevMode;

	FILE *f;
	char filePath[50] = "e:\\Printer";
	filePath[strlen(filePath)] =(char) type+'0';
	filePath[strlen(filePath)+1] = 0;
	fopen_s(&f,filePath,"rb");
	if(!f)
		return 0;
	fread(&diskP,sizeof(diskP),1,f);
	fread(&nDevMode,sizeof(nDevMode),1,f);
	free(pDevMode);
	pDevMode =(LPDEVMODE) malloc(nDevMode);
	fread(pDevMode,nDevMode,1,f);

	free(printerInfo.pDriverName);
	free(printerInfo.pPrinterName);
	free(printerInfo.pPortName);
	printerInfo.pDriverName =str::Dup(diskP.DriverName);
	printerInfo.pPrinterName = str::Dup(diskP.PrinterName);
	printerInfo.pPortName = str::Dup(diskP.PortName);
	fclose(f);
	return 1;
}
void InitPrintDlg(HWND hDlg,WindowInfo* win)   //设置4组全局变量 gPrinterInfo gpDevMode gIsReady gRangesc
{                                            //函数功能可细分 dlginit与prepare data
	int i;
	WCHAR text[50] = {0};
	for (i=0;i<PNUM;i++)
	{
		if(GetPrinterInfo(i+3,gPrinterInfo[i],gpDevMode[i]))
		{
			wcscpy_s(text,gPrinterInfo[i].pPrinterName);
			gIsReady[i] = 1;
		}else
		{
			wcscpy_s(text,L"打印机未设置");
			gIsReady[i] = 0;
		}
		switch(i)
		{
		case 0:
			SetDlgItemText(hDlg, IDC_P3, text);
			break;
		case 1:
			SetDlgItemText(hDlg, IDC_P4, text);
			break;
		case 2:
			SetDlgItemText(hDlg, IDC_P5, text);
			break;
		}
	}

	gRangesc.init(hDlg,win);
	gRangesc.Classify();
	gRangesc.Show();



}



static INT_PTR CALLBACK PrintDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	HWND parent = GetParent(hDlg);
	WindowInfo *win = FindWindowInfoByHwnd(hDlg);
	//WindowInfo *win = FindWindowInfoByHwnd(parent);
	switch (msg)
	{
	case WM_INITDIALOG:
		InitPrintDlg(hDlg,win);
		return FALSE;
	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDOK:
			pan_OnPrint(win);
			EndDialog(hDlg, IDOK);
			return TRUE;

		case IDCANCEL:
			EndDialog(hDlg, IDCANCEL);
			return TRUE;
		case IDC_BUTTON3:
			{
				PRINTDLGEX pd;
				GetPdFromUser(pd,win);
				TransformAndSavePd(3,pd,hDlg);
				InitPrintDlg( hDlg,win);
				return TRUE;
			}
			
		case IDC_BUTTON4:
			{
				PRINTDLGEX pd;
				GetPdFromUser(pd,win);
				TransformAndSavePd(4,pd,hDlg);
				InitPrintDlg( hDlg,win);
				return TRUE;
			}
		case IDC_BUTTON5:
			{
				PRINTDLGEX pd;
				GetPdFromUser(pd,win);
				TransformAndSavePd(5,pd,hDlg);
				InitPrintDlg( hDlg,win);
				return TRUE;
			}
		}
		break;
	}
	return FALSE;
}


void pan_PrintDlg_bak(WindowInfo *win)
{
	if(DialogBoxParam(NULL, MAKEINTRESOURCE(IDD_PAN_PRINT), win->hwndFrame, PrintDlgProc, NULL)!= IDOK)
	{
		return;
	}
}

void static formatListItemString(wchar_t *title,int len,wchar_t* rangeName,wchar_t * printerName)
{
	if(rangeName)
	{
		wcscat_s(title,len,L"     ");
		wcscat_s(title,len,rangeName);
	}
	if(printerName)
	{
		wcscat_s(title,len,L"      ");
		wcscat_s(title,len,printerName);
	}
}
void OnDlgInit(HWND hDlg,WindowInfo *win)
{
	int i;
	unsigned int j;
	wchar_t * rangeName;
	wchar_t * printerName;
	WCHAR title[100];
	HWND list = GetDlgItem(hDlg,IDC_LIST2);
	int pageNum = win->dm->PageCount();
	unsigned int printerNum = printContext->printers.Size();
	for (i = 1; i <= pageNum; i++)          //page与 range、printer是多对一关联。
	{
		rangeName = NULL;
		printerName = NULL;
		j = printContext->getPagePrinterNum(i);
		rangeName = printContext->pageInfos[i].sizeName;
		printerName = printContext->printers[j]->printerName;
		swprintf_s(title,sizeof(title)/sizeof(WCHAR) - 1 ,L"第 %d 页",i);
		formatListItemString(title,100,rangeName,printerName);
		
		ListBox_AppendString_NoSort(list,title);
	}

	for (i = 0; i< int(printerNum);i++ )
	{
		SendDlgItemMessage(hDlg, IDC_COMBO1, CB_INSERTSTRING, (WPARAM)-1, (LPARAM)printContext->printers[i]->printerName);
	}

	SendDlgItemMessage(hDlg, IDC_LIST2, LB_SETCURSEL, 0, 0);
	i = printContext->getPagePrinterNum(1);
	SendDlgItemMessage(hDlg, IDC_COMBO1, CB_SETCURSEL, i , 0);
	
}

static void OnPrinterAlter(HWND hDlg)
{
	int printerNum,newPrinterNum,page = 0;
	page = ListBox_GetCurSel(GetDlgItem(hDlg, IDC_LIST2))+1;
	printerNum = printContext->getPagePrinterNum(page);
	newPrinterNum = SendDlgItemMessage(hDlg, IDC_COMBO1, CB_GETCURSEL, 0, 0);
	if(printerNum != -1)
		printContext->pageRanges[printerNum]->set(page,0);
	printContext->pageRanges[newPrinterNum]->set(page,1);


	wchar_t title[100];
	wchar_t *rangeName = printContext->pageInfos[page].sizeName;
	wchar_t *printerName = printContext->printers[newPrinterNum]->printerName;
	swprintf_s(title,sizeof(title)/sizeof(WCHAR) - 1 ,L"第 %d 页",page);
	formatListItemString(title,100,rangeName,printerName);

	HWND listHwnd = GetDlgItem(hDlg,IDC_LIST2);
	ListBox_DeleteString(listHwnd,page - 1);
	ListBox_InsertString(listHwnd,page - 1,title);
	ListBox_SetCurSel(listHwnd,page - 1);
	SetDlgItemText(hDlg,IDC_ALTER_TEXT,L"修改成功。");
	
}
static void OnListClick(HWND hDlg)
{
	int j;
	HWND pageList;
	int idx;
	pageList = GetDlgItem(hDlg, IDC_LIST2);
	idx = ListBox_GetCurSel(pageList);
	j = printContext->getPagePrinterNum(idx+1);
	SendDlgItemMessage(hDlg, IDC_COMBO1, CB_SETCURSEL, j , 0);
	SetDlgItemText(hDlg,IDC_ALTER_TEXT,L"");
}

static void OnOK(HWND hDlg,WindowInfo * win)
{
	pan_OnprintEx(win);
}
static void OnPaint(HWND hDlg,WindowInfo *win)
{


}
static void OnSetting(HWND hDlg,WindowInfo * win)
{

}

 static INT_PTR CALLBACK pan_PrintDlgProc(HWND hDlg,UINT msg, WPARAM wParam,LPARAM lParam )
{
	WindowInfo *win = FindWindowInfoByHwnd(hDlg);
	switch(msg)
	{
	case WM_INITDIALOG:
		printContext->init(win,hDlg);
		OnDlgInit(hDlg,win);
		return FALSE;
	case WM_PAINT:
		OnPaint(hDlg,win);
		return FALSE;

	case WM_COMMAND:
		switch(LOWORD(wParam))
		{
		case IDOK:
			/*wchar_t str[20];
			pageList = GetDlgItem(hDlg, IDC_LIST2);
			idx = ListBox_GetCurSel(pageList);
			_itow_s(idx,str,10);
			MessageBox(hDlg,str,L"x",0);
			EndDialog(hDlg, IDOK);*/
			OnOK(hDlg,win);
			EndDialog(hDlg,IDOK);
			return TRUE;
		case IDCANCEL:
			EndDialog(hDlg, IDCANCEL);
			return TRUE;
		case IDC_LIST2:
			switch(HIWORD(wParam))
			{
			case 1:
				OnListClick(hDlg);
			}
			break;
		case IDC_SETTING:
			OnSetting(hDlg,win);
			break;
		case IDC_ALTER:
			OnPrinterAlter(hDlg);
			break;
			
		default:
			
			break;
		}
		break;
	}
	return FALSE;
}

void pan_PrintDlg(WindowInfo *win)
{
	pan_PrintContext context;
	printContext = &context;
	if(DialogBoxParam(NULL, MAKEINTRESOURCE(IDD_PRINT), win->hwndFrame, pan_PrintDlgProc, NULL)== IDOK)
	{
		
	}

	printContext = NULL;
	
	
}



PrintData * pan_CreatePrintData(pan_Printer * printer,WindowInfo *win)
{
	Print_Advanced_Data advanced(PrintRangeAll, PrintScaleShrink, false);
	PrintData *data = new PrintData(win->dm->engine, &(printer->printerInfo), printer->pDevMode, 
		printer->pageRange->ppr, advanced, win->dm->Rotation(), NULL);
	return data;
}
void pan_OnprintEx(WindowInfo *win)
{
	if(printContext->isPrinting)
	{
		MessageBoxW(win->hwndFrame,L"正在打印,请稍后再试",L"提示",0);
		return ;
	}
	if (!HasPermission(Perm_PrinterAccess)) return;
	DisplayModel *dm = win->dm;
	assert(dm);
	if (!dm) return;
	if (!dm->engine)
		return;
#ifndef DISABLE_DOCUMENT_RESTRICTIONS
	if (!dm->engine->AllowsPrinting())
		return;
#endif
	if (win->IsChm()) {
		win->dm->AsChmEngine()->PrintCurrentPage();
		return;
	}
	if (win->printThread) {
		int res = MessageBox(win->hwndFrame, 
			_TR("Printing is still in progress. Abort and start over?"),
			_TR("Printing in progress."),
			MB_ICONEXCLAMATION | MB_YESNO | MbRtlReadingMaybe());
		if (res == IDNO)
			return;
	}
	AbortPrinting(win);


	int printerNum = printContext->printers.Size();
	int i;
	PrintData ** datas;
	datas = new PrintData *[printerNum];
	for (size_t i = 0;i<printContext->pageRanges.Size();i++)
		printContext->pageRanges[i]->format();

	for (i = 0;i<printerNum;i++)
	{
		if(printContext->printers[i]->isReady && !printContext->printers[i]->pageRange->isEmpty())
			datas[i] = pan_CreatePrintData(printContext->printers[i],win);
		else
			datas[i] = NULL;
	}

	PrinttingControlThreadData *threadData;
	threadData = new PrinttingControlThreadData();
	threadData->win = win;
	threadData->data = datas;
	threadData->len = printerNum;
	HANDLE hwnd = CreateThread(NULL, 0, PrinttingControlThread, threadData, 0, NULL);
	CloseHandle(hwnd);
}

void  pan_OnPrint(WindowInfo *win)
{
	if(gIsPrinting)
	{
		MessageBoxW(win->hwndFrame,L"正在打印,请稍后再试",L"提示",0);
		return ;
	}
    if (!HasPermission(Perm_PrinterAccess)) return;

    DisplayModel *dm = win->dm;
    assert(dm);
    if (!dm) return;

    if (!dm->engine)
        return;
#ifndef DISABLE_DOCUMENT_RESTRICTIONS
    if (!dm->engine->AllowsPrinting())
        return;
#endif

    if (win->IsChm()) {
        win->dm->AsChmEngine()->PrintCurrentPage();
        return;
    }

    if (win->printThread) {
        int res = MessageBox(win->hwndFrame, 
                             _TR("Printing is still in progress. Abort and start over?"),
                             _TR("Printing in progress."),
                             MB_ICONEXCLAMATION | MB_YESNO | MbRtlReadingMaybe());
        if (res == IDNO)
            return;
    }
    AbortPrinting(win);



	int i;
	PrintData ** datas;
	datas = new PrintData *[PNUM];
	int nRange;
	PRINTPAGERANGE * pRange = NULL;
	for (i = 0;i<PNUM;i++)
	{
		pRange = gRangesc.getRanges((i+3 == 5)?0:i+3,nRange);
		if(nRange>0 && gIsReady[i])
			datas[i] = CreatePrintData(gPrinterInfo[i],gpDevMode[i],pRange,nRange,win);
		else
			datas[i] = NULL;
	}


	PrinttingControlThreadData *threadData;
	threadData = new PrinttingControlThreadData();
	threadData->win = win;
	threadData->data = datas;
	threadData->len = i;


	HANDLE hwnd = CreateThread(NULL, 0, PrinttingControlThread, threadData, 0, NULL);
	CloseHandle(hwnd);
	
}


//static void OnSetting(HWND hDlg,WindowInfo *win)
//{
//	
//
//
//}

