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

class pan_PrintContext;
class pan_PageInfo;
class pan_PrintContext;

static HANDLE pan_PrintMutex;
HWND pan_printDlgHwnd;
static WindowInfo *pan_win;
pan_PrintContext *printContext;
void pan_PrintSettingDlg(HWND hParent);

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

struct DiskPrinterInfo
{
	WCHAR DriverName[200];
	WCHAR PrinterName[200];
	WCHAR PortName[200];
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
		printerName = L"未指定打印机";
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
		{
			isReady = 0;
			return 0;
		}
		
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
		{
			isReady = 0;
			return 0;
		}
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
		//free(filePath);
		printerInfo.pDriverName = NULL;
		printerInfo.pPrinterName = NULL;
		printerInfo.pPortName = NULL;
		pDevMode = NULL;
		//filePath = NULL;
		isReady = 0;
		printerName = L"未指定打印机";
	}
	void removeFile()
	{
		DeleteFile(filePath);
	}
	PRINTER_INFO_2 printerInfo;
	LPDEVMODE pDevMode;

	int isReady;
	WCHAR * filePath;  //应该放到外面
	WCHAR * printerName;

};

class pan_PageRange   //页码从1开始
{
	char page[1000];
	std::vector<pan_PageInfo> * pageInfos;
public:
	Vec<PRINTPAGERANGE> ppr;
	pan_PageRange(std::vector<pan_PageInfo> * pageInfos)
	{
		memset(page,0,sizeof(page));
		this->pageInfos = pageInfos;
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
		for (i=1;i< (int)pageInfos->size();i++)//页码从1开始
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

class pan_PageSize
{
public:
	double a;
	double b;
	wchar_t typeName[20];
	wchar_t sizeName[20];
};
class pan_PageInfo
{
public:
	char printType;
	char rawType;
};
class pan_PrintContext
{
	wchar_t * pageSettingPath;
public:
	Vec<pan_PageRange*> pageRanges;   //有点多余
	Vec<pan_PageSize> pageSizes;
	Vec<pan_Printer*> printers;
	std::vector<pan_PageInfo> pageInfos;  //Vec不支持resize，用vector， 1 based
	int isInit;
	int isPrinting;
	pan_PrintContext()
	{
		isPrinting = 0;
		isInit = 0;
	}
	void init()
	{
		if(isInit == 1)
			return ;
		isInit = 1;
		addDefaultPageSize();
		generatePageInfo();
		DefaultPrintType();
		generatePageRange();
		loadPrinters();
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
	int addPageSize(const pan_PageSize& ps)
	{
		for (unsigned i = 0;i< pageSizes.Size();i++)
		{
			if (wcscmp(ps.typeName,pageSizes[i].typeName) == 0)
				return 0;
		}
		pan_PageRange * pageRange;
		pan_Printer * printer;

		pageSizes.Append(ps);

		pageRange = new pan_PageRange(&pageInfos);
		pageRanges.Append(pageRange);

		
		printer = new pan_Printer();
		wchar_t curDir[200];
		GetCurrentDirectory(sizeof(curDir),curDir);
		printer->setFilePath(str::Format(L"%s\\printer%d",curDir,pageSizes.Size()-1));
		printers.Append(printer);
		return 1;
	}
	void removePrinter(unsigned idx)
	{
		printers[idx]->removeFile();
		printers[idx]->reset();
	}

	int modifyPageSize(unsigned idx,const pan_PageSize & ps)
	{
		for (unsigned i = 0;i< pageSizes.Size();i++)
		{
			if (idx!= i && wcscmp(ps.typeName,pageSizes[i].typeName) == 0)
				return 0;
		}
		pageSizes[idx] = ps;
		return 1;
	}

	int getPagePrintType(int page)
	{
		return pageInfos[page].printType;
	}
	int getPageRawType(int page)
	{
		return pageInfos[page].rawType;
	}
	int setPagePrintType(int page,int type)
	{
		if(type<0 || type >= (int)pageSizes.Size())
			return 0;
		pageInfos[page].printType = (char)type;
		
		return 1;
	}
	int isReady(int i)
	{
		if(i>=0 && i< static_cast<int>(pageSizes.Size()))
			return (printers[i]->isReady && !pageRanges[i]->isEmpty());
		return 0;
	}

	int loadPageSizeAndPageInfo(wchar_t *path)
	{
		FILE *fp;
		size_t len,i;
		pan_PageSize ps;
		pan_PageInfo pi;
		_wfopen_s(&fp,path,L"rb");
		if(!fp)
			return 0;
		pageSizes.Reset();
		pageInfos.clear();
		fwscanf_s(fp,L"%u",&len);
		for (i=0;i<len;i++)
		{
			fread(&ps,sizeof(pan_PageSize),1,fp);
			pageSizes.Push(ps);
		}
		fwscanf_s(fp,L"%u",&len);
		for (i=0;i<len;i++)
		{
			pi.printType = (char)getc(fp);
			pageInfos.push_back(pi);
		}
		fclose(fp);
		return 1;

	}
	int savePageSizeAndPageInfo(wchar_t *path)
	{
		FILE * fp;
		size_t i;
		_wfopen_s(&fp,path,L"wb");
		if(!fp)
			return 0;
		fwprintf_s(fp,L"%u",pageRanges.Size());
		for (i = 0;i<pageSizes.Size();i++)
		{
			fwrite(&pageSizes[i],sizeof(pan_PageSize),1,fp);
		}
		fwprintf_s(fp,L"%u",pageInfos.size());
		for (i=0;i< pageInfos.size();i++)   //页码从1开始 到size-1,数据从0开始 到size-1
		{
			fputc(pageInfos[i].printType,fp);
		}
		fclose(fp);
		return 1;
	}
	void generatePageRange()
	{
		unsigned int i;
		int j;
		for (i = 0;i < pageRanges.Size();i++)
		{
			pageRanges[i]->clear();
		}
		for (i = 1;i < pageInfos.size(); i++)
		{
			j = pageInfos[i].printType;
			pageRanges[j]->set(i,1);
		}
		for (i = 0;i<pageRanges.Size();i++)
		{
			pageRanges[i]->format();
		}
	}
	int getPrintPagesCount()
	{
		return pageInfos.size() -1;
	}
	void DefaultPrintType()
	{
		for (size_t i = 1; i < pageInfos.size();i++ )
			pageInfos[i].printType = pageInfos[i].rawType;
	}
	void generatePageInfo()  //与PDF相关函数
	{
		if(pan_win == NULL)
			return;         //出错
		DisplayModel * dm = pan_win->dm;
		int i,j;
		pageInfos.clear();
		pageInfos.resize(dm->PageCount() + 1);
		for (i=1;i<=dm->PageCount();i++ )
		{
			j = getPageTypeFromPDF(i,dm);
			pageInfos[i].rawType = (char)j;
		}
	}

private:
	void addDefaultPageSize()
	{
		pan_PageSize ps;
		ps.a = 0;
		ps.b = 0;
		wcscpy_s(ps.typeName,_countof(ps.typeName),L"其他");
		wcscpy_s(ps.sizeName,_countof(ps.sizeName),L"other");
		addPageSize(ps);

		ps.a = 42;
		ps.b = 29.7;
		wcscpy_s(ps.sizeName,_countof(ps.sizeName),L"A3");
		wcscpy_s(ps.typeName,_countof(ps.typeName),L"A3黑白");
		addPageSize(ps);
		ps.a = 21;
		ps.b = 29.7;
		wcscpy_s(ps.sizeName,_countof(ps.sizeName),L"A4");
		wcscpy_s(ps.typeName,_countof(ps.typeName),L"A4黑白");
		addPageSize(ps);
		ps.a = 14.8;
		ps.b = 21;
		wcscpy_s(ps.sizeName,_countof(ps.sizeName),L"A5");
		wcscpy_s(ps.typeName,_countof(ps.typeName),L"A5黑白");
		addPageSize(ps);


		ps.a = 42;
		ps.b = 29.7;
		wcscpy_s(ps.sizeName,_countof(ps.sizeName),L"A3");
		wcscpy_s(ps.typeName,_countof(ps.typeName),L"A3彩色");
		addPageSize(ps);
		ps.a = 21;
		ps.b = 29.7;
		wcscpy_s(ps.sizeName,_countof(ps.sizeName),L"A4");
		wcscpy_s(ps.typeName,_countof(ps.typeName),L"A4彩色");
		addPageSize(ps);
		ps.a = 14.8;
		ps.b = 21;
		wcscpy_s(ps.sizeName,_countof(ps.sizeName),L"A5");
		wcscpy_s(ps.typeName,_countof(ps.typeName),L"A5彩色");
		addPageSize(ps);
	}

	int getPageTypeFromPDF(int num,DisplayModel * dm)
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

	void loadPrinters()
	{
		size_t i;
		wchar_t curDir[200];
		GetCurrentDirectory(sizeof(curDir),curDir);
		for (i = 0;i < printers.Size(); i++)
		{
			printers[i]->setFilePath(str::Format(L"%s\\printer%d",curDir,i));
			printers[i]->loadPrinterFromFile();
		}
	}
	
};

class ScopeHDC {
	HDC hdc;
public:
	ScopeHDC(HDC hdc) : hdc(hdc) { }
	~ScopeHDC() { DeleteDC(hdc); }
	operator HDC() const { return hdc; }
};

static RectD BoundSelectionOnPage(const Vec<SelectionOnPage>& sel, int pageNo)
{
	RectD bounds;
	for (size_t i = 0; i < sel.Count(); i++) {
		if (sel.At(i).pageNo == pageNo)
			bounds = bounds.Union(sel.At(i).rect);
	}
	return bounds;
}

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

class PrintingControlThreadData
{
public:
	WindowInfo *win;
	PrintData **data;
	int len;
	PrintingControlThreadData()
	{
		win = NULL;
		data = NULL;
		len = 0;
	}
	~PrintingControlThreadData()
	{
		delete [] data;
	}
};

static int gisPrinting = 0;
static DWORD WINAPI PrinttingControlThread(LPVOID inData)
{
	gisPrinting = 1;
	PrintingControlThreadData *treadData = (PrintingControlThreadData*) inData;
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
	gisPrinting = 0;
	return 0;
}

static void extendStr(WCHAR * str,unsigned int len)
{
	unsigned olen = wcslen(str);
	unsigned i;
	for (i = olen;i<len;i++)
	{
		str[i] = ' ';
	}
	if (i == len)
	{
		str[i] = 0;
	}
}

#define  showListItemCode \
swprintf_s(str,_countof(str),L" 第 %d",page); \
extendStr(str,7); \
len = wcslen(str); \
swprintf_s(str+len,_countof(str)-len,L"页          %s：",psr.sizeName); \
extendStr(str,23); \
len = wcslen(str); \
swprintf_s(str+len,_countof(str)-len,L"%0.2lf x %0.2lf",psr.a,psr.b); \
extendStr(str,55); \
len = wcslen(str); \
swprintf_s(str+len,_countof(str)-len,L"%s",ps.typeName); 

static void addPage(int page,HWND hDlg)
{
	unsigned int type;
	pan_PageSize ps,psr;
	wchar_t str[100];
	int len;
	HWND list = GetDlgItem(hDlg,IDC_LIST2);
	type = printContext->getPagePrintType(page);
	ps = printContext->pageSizes[type];
	type = printContext->getPageRawType(page);
	psr = printContext->pageSizes[type];
	
	showListItemCode

	ListBox_AppendString_NoSort(list,str);
}

void showGroupText(HWND hDlg,int page)
{
	wchar_t str[100];
	int printType = printContext->getPagePrintType(page);
	swprintf_s(str,_countof(str),L"第%d页打印信息",page);
	SetDlgItemText(hDlg,IDC_GROUP_TEXT,str);
	SetDlgItemText(hDlg,IDC_PRINTER_TEXT,printContext->printers[printType]->printerName);
}

static void showPageList(HWND hDlg)
{
	int i;
	SendDlgItemMessage(hDlg,IDC_LIST2,LB_RESETCONTENT,0,0);
	int pageNum = printContext->getPrintPagesCount();
	for (i = 1; i <= pageNum; i++)
	{
		addPage(i,hDlg);
	}
	SendDlgItemMessage(hDlg, IDC_LIST2, LB_SETCURSEL, 0, 0);
	i = printContext->getPagePrintType(1);
	SendDlgItemMessage(hDlg, IDC_COMBO1, CB_SETCURSEL, i , 0);

	showGroupText(hDlg,1);
}
static void OnChangePage(HWND hDlg)
{
	int newType,page = 0;
	int len;
	page = ListBox_GetCurSel(GetDlgItem(hDlg, IDC_LIST2))+1;
	newType = SendDlgItemMessage(hDlg, IDC_COMBO1, CB_GETCURSEL, 0, 0);
	printContext->setPagePrintType(page,newType);

	wchar_t str[100];
	pan_PageSize ps = printContext->pageSizes[newType];
	int rawType = printContext->getPageRawType(page);
	pan_PageSize psr = printContext->pageSizes[rawType];

	showListItemCode

	wchar_t *printerName = printContext->printers[newType]->printerName;
	SetDlgItemText(hDlg,IDC_PRINTER_TEXT,printerName);

	HWND listHwnd = GetDlgItem(hDlg,IDC_LIST2);
	ListBox_DeleteString(listHwnd,page - 1);
	ListBox_InsertString(listHwnd,page - 1,str);
	ListBox_SetCurSel(listHwnd,page - 1);
	SetDlgItemText(hDlg,IDC_ALTER_TEXT,L"修改成功。");

}

static void showTypeCombo(HWND hDlg)
{
	wchar_t str[200];
	int len;
	SendDlgItemMessage(hDlg,IDC_COMBO1,CB_RESETCONTENT,0,0);
	unsigned int sizeNum = printContext->pageSizes.Size();
	for (unsigned i = 0; i< sizeNum;i++ )
	{
		memset(str,0,sizeof(str));
		wcscat_s(str,printContext->pageSizes[i].typeName);
		extendStr(str,6);
		len = wcslen(str);
		swprintf_s(str+len,_countof(str) - len,L"%0.2lf x %0.2lf",printContext->pageSizes[i].a,printContext->pageSizes[i].b);
		SendDlgItemMessage(hDlg, IDC_COMBO1, CB_INSERTSTRING, (WPARAM)-1, (LPARAM)str);
	}
}
static void OnDlgInit(HWND hDlg)
{
	//HICON hicon = LoadIcon(NULL,MAKEINTRESOURCE(IDI_ICON1));
	//SendMessage(hDlg,WM_SETICON,ICON_BIG,(LPARAM)hicon);
	showTypeCombo(hDlg);
	showPageList(hDlg);
}


static void OnListClick(HWND hDlg)
{
	int type;
	HWND pageList;
	int idx;
	pageList = GetDlgItem(hDlg, IDC_LIST2);
	idx = ListBox_GetCurSel(pageList);
	type = printContext->getPagePrintType(idx+1);
	SendDlgItemMessage(hDlg, IDC_COMBO1, CB_SETCURSEL, type , 0);
	SetDlgItemText(hDlg,IDC_ALTER_TEXT,L"");
	showGroupText(hDlg,idx+1);
}
void pan_showPDF(int page);
static void OnListDoubleClick(HWND hDlg)
{
	HWND pageList;
	int page;
	pageList = GetDlgItem(hDlg, IDC_LIST2);
	page = ListBox_GetCurSel(pageList)+1;
	pan_showPDF(page);
}

static void OnSetting(HWND hDlg)
{
	pan_PrintSettingDlg(hDlg);
}
static void OnSavePage(HWND hDlg)
{
	wchar_t filePath[200] = {0};
	wchar_t curDir[200];
	OPENFILENAME ofn = {sizeof(OPENFILENAME)};
	ofn.hwndOwner = hDlg;
	ofn.lpstrFile = filePath;
	ofn.nMaxFile = sizeof(filePath);
	ofn.lpstrTitle = L"保存页配置";
	GetCurrentDirectory(sizeof(curDir),curDir);
	ofn.lpstrInitialDir = curDir;
	ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST;
	//ofn.lpstrDefExt = pan_win->loadedFilePath;
	ofn.lpstrFilter = L"页配置文件(*.ppag)\0*.ppag\0所有文件(*.*)\0*.*\0\0";
	if(GetSaveFileName(&ofn) == 0)
		return;

	wcscat_s(filePath,L".ppag");
	printContext->savePageSizeAndPageInfo(filePath);
}
static void OnLoadPage(HWND hDlg)
{
	wchar_t filePath[200] = {0};
	wchar_t curDir[200];
	OPENFILENAME ofn = {sizeof(OPENFILENAME)};
	ofn.hwndOwner = hDlg;
	ofn.lpstrFile = filePath;
	ofn.nMaxFile = sizeof(filePath);
	ofn.lpstrTitle = L"打开页配置";
	GetCurrentDirectory(sizeof(curDir),curDir);
	ofn.lpstrInitialDir = curDir;
	ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST;
	ofn.lpstrFilter = L"页配置文件(*.ppag)\0*.ppag\0所有文件(*.*)\0*.*\0\0";
	if(GetOpenFileName(&ofn) == 0)
		return;
	printContext->loadPageSizeAndPageInfo(filePath);
	if (printContext->getPrintPagesCount() != pan_win->dm->PageCount())
	{
		MessageBox(hDlg,L"配置文件与PDF文档不匹配",L"注意",0);
	}
	showPageList(hDlg);
}

static void OnDefaultPage(HWND hDlg)
{
	printContext->DefaultPrintType();
	showPageList(hDlg);
}

static void OnClose(HWND hDlg)
{
	DestroyWindow(hDlg);
	delete printContext;
	printContext = NULL;
	pan_printDlgHwnd = NULL;
}
int pan_DoPrint(HWND);
static void OnOK(HWND hDlg)
{
	if(pan_DoPrint(hDlg))
	{
		OnClose(hDlg);
	}
}
 static INT_PTR CALLBACK pan_PrintDlgProc(HWND hDlg,UINT msg, WPARAM wParam,LPARAM lParam )
{
	WindowInfo *win = pan_win;
	switch(msg)
	{
	case WM_INITDIALOG:
		printContext = new pan_PrintContext();
		printContext->init();
		OnDlgInit(hDlg);
		return TRUE;
	case WM_CLOSE :
		OnClose(hDlg);
		return TRUE;
	case WM_COMMAND:
		switch(LOWORD(wParam))
		{
		case IDOK:
			OnOK(hDlg);
			break;
		case IDCANCEL:
			OnClose(hDlg);
			break;
		case IDC_LIST2:
			switch(HIWORD(wParam))
			{
			case LBN_SELCHANGE:
				OnListClick(hDlg);
				break;
			case LBN_DBLCLK:
				OnListDoubleClick(hDlg);
			}
			break;
		case IDC_SETTING:
			OnSetting(hDlg);
			break;
		case IDC_ALTER:
			OnChangePage(hDlg);
			break;
		case IDC_LOAD_PAGE:
			OnLoadPage(hDlg);
			break;
		case IDC_SAVE_PAGE:
			OnSavePage(hDlg);
			break;
		case IDC_DEFAULT_PAGE:
			OnDefaultPage(hDlg);
		case IDC_COMBO1:
			return TRUE;
		default:
			;
		}
		return TRUE;
	}
	return FALSE;
}

 DWORD WINAPI pan_PrintDlgThread(LPVOID)
{
	pan_printDlgHwnd = CreateDialog(NULL,MAKEINTRESOURCE(IDD_PRINT),NULL,pan_PrintDlgProc);
	ShowWindow(pan_printDlgHwnd,SW_SHOW);
	MSG msg = { 0 };
	while (GetMessage(&msg, NULL, 0, 0)) {

		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	return 0;
}
void pan_PrintDlg(WindowInfo *win)
{
	if(pan_printDlgHwnd != NULL)
	{
		SetForegroundWindow(pan_printDlgHwnd);
		return;
	}
	pan_win = win;
	HANDLE hwnd = CreateThread(NULL, 0, pan_PrintDlgThread, NULL, 0, NULL);
	CloseHandle(hwnd);
}

PrintData * pan_CreatePrintData(pan_Printer * printer,pan_PageRange *pageRange,WindowInfo *win)
{
	Print_Advanced_Data advanced(PrintRangeAll, PrintScaleShrink, false);
	PrintData *data = new PrintData(win->dm->engine, &(printer->printerInfo), printer->pDevMode, 
		pageRange->ppr, advanced, win->dm->Rotation(), NULL);
	return data;
}


int pan_DoPrint(HWND hDlg)
{
	WindowInfo * win = pan_win;
	if(gisPrinting)
	{
		MessageBox(hDlg,L"正在打印,请稍后再试",L"提示",0);
		return 0;
	}
	if (printContext->getPrintPagesCount() != pan_win->dm->PageCount())
	{
		MessageBox(hDlg,L"配置文件与PDF文档不匹配，不能打印",L"注意",0);
		return 0;
	}
	if (!HasPermission(Perm_PrinterAccess)) return 0;
	DisplayModel *dm = win->dm;
	assert(dm);
	if (!dm) return 0;
	if (!dm->engine)
		return 0;
#ifndef DISABLE_DOCUMENT_RESTRICTIONS
	if (!dm->engine->AllowsPrinting())
		return 0;
#endif
	if (win->IsChm()) {
		win->dm->AsChmEngine()->PrintCurrentPage();
		return 0;
	}
	
	int printerNum = printContext->printers.Size();
	PrintData ** datas;
	datas = new PrintData *[printerNum];
	for (size_t i = 0;i<printContext->pageRanges.Size();i++)
		printContext->generatePageRange();

	for (int i = 0;i<printerNum;i++)
	{
		if(printContext->isReady(i))
			datas[i] = pan_CreatePrintData(printContext->printers[i],printContext->pageRanges[i],win);
		else
			datas[i] = NULL;
	}

	PrintingControlThreadData *threadData;
	threadData = new PrintingControlThreadData();
	threadData->win = win;
	threadData->data = datas;
	threadData->len = printerNum;


	HANDLE hwnd = CreateThread(NULL, 0, PrinttingControlThread, threadData, 0, NULL);
	CloseHandle(hwnd);
	return 1;
}
void pan_showPDF(int page)
{
	WindowInfo *win = pan_win;
	if (win->dm->ValidPageNo(page)) 
	{
		win->dm->GoToPage(page, 0, true);
		SetForegroundWindow(win->hwndFrame);
	}
}

////////////////////////////////////////////////////////////////////////
static void addSize_ps(int i,HWND hDlg)
{
	double x,y;
	wchar_t str[200];
	wchar_t *sizeName, *printerName;
	HWND list = GetDlgItem(hDlg,IDC_PS_LIST);
	int len;
	sizeName = printContext->pageSizes[i].typeName;
	x = printContext->pageSizes[i].a;
	y = printContext->pageSizes[i].b;
	printerName = printContext->printers[i]->printerName;
	swprintf_s(str,_countof(str),L"%d.  %s",i+1,sizeName);
	extendStr(str,15);
	len = wcslen(str);
	swprintf_s(str+len,_countof(str) - len,L"%0.2lf x %0.2lf",x,y);
	extendStr(str,43);
	len = wcslen(str);
	swprintf_s(str+len,_countof(str) - len,L"%s",printerName);
	ListBox_AppendString_NoSort(list,str);
}
static void showSizeList(HWND hDlg)
{
	unsigned i;
	SendDlgItemMessage(hDlg,IDC_PS_LIST,LB_RESETCONTENT,0,0);
	for (i=0;i<printContext->pageSizes.Size();i++)
	{
		addSize_ps(i,hDlg);
	}
}

static void OnSeletePrinter_ps(HWND hDlg)
{
	PRINTDLGEX pd;
    ZeroMemory(&pd, sizeof(PRINTDLGEX));
    pd.lStructSize = sizeof(PRINTDLGEX);
    pd.hwndOwner   = hDlg;
    
	pd.Flags       = PD_USEDEVMODECOPIESANDCOLLATE | PD_COLLATE;
    
    pd.Flags |= PD_NOSELECTION;
	pd.Flags |= PD_NOCURRENTPAGE;
	pd.Flags |= PD_NOPAGENUMS;
    pd.nCopies     = 1;
    pd.nStartPage = START_PAGE_GENERAL;
    pd.nPropertyPages = 0;

    if (PrintDlgEx(&pd) != S_OK) {
        if (CommDlgExtendedError() != 0) {
            MessageBoxWarning(hDlg, _TR("Couldn't initialize printer"), 
                              _TR("Printing problem."));
        }
		GlobalFree(pd.hDevNames);
		GlobalFree(pd.hDevMode);
		return;
    }

    if (pd.dwResultAction != PD_RESULT_PRINT)
	{
		GlobalFree(pd.hDevNames);
		GlobalFree(pd.hDevMode);
		return;
	}

	int type;
	type = ListBox_GetCurSel(GetDlgItem(hDlg, IDC_PS_LIST));
	printContext->printers[type]->loadPrinterFromPd(pd);
	printContext->printers[type]->savePrinterToFile();
	showSizeList(hDlg);
	SendDlgItemMessage(hDlg,IDC_PS_LIST,LB_SETCURSEL,(WPARAM)type,0);
}

static int isDouble(wchar_t * str)
{
	for(;*str!=0;++str)
	{
		if((*str < L'0' || *str>L'9') && *str != L'.')
			return 0;
	}
	return 1;
}

static int OnAdd_ps(HWND hDlg)
{
	wchar_t  name[200];
	wchar_t strx[20];
	wchar_t stry[20];
	double x,y;
	pan_PageSize ps;
	SendDlgItemMessage(hDlg,IDC_PS_NAME,WM_GETTEXT,(WPARAM)200,(LPARAM)name);
	SendDlgItemMessage(hDlg,IDC_PS_X,WM_GETTEXT,(WPARAM)20,(LPARAM)strx);
	SendDlgItemMessage(hDlg,IDC_PS_Y,WM_GETTEXT,(WPARAM)20,(LPARAM)stry);
	if (name[0] == 0 || strx[0] == 0 || stry[0] == 0 || !isDouble(strx) || !isDouble(stry))
	{
		MessageBox(hDlg,L"参数错误",L"添加失败",0);
		return 0;
	}
	wcscpy_s(ps.typeName,name);
	swscanf_s(strx,L"%lf",&x);
	swscanf_s(stry,L"%lf",&y);
	ps.a = x;
	ps.b = y;
	
	if(!printContext->addPageSize(ps))
	{
		MessageBox(hDlg,L"名字重复",L"添加失败",0);
		return 0;
	}
	showSizeList(hDlg);
	SendDlgItemMessage(hDlg,IDC_PS_LIST,LB_SETCURSEL,(WPARAM)printContext->pageSizes.Size()-1,0);
	return 1;
}
static int OnModify_ps(HWND hDlg)
{
	wchar_t  name[200];
	wchar_t strx[20];
	wchar_t stry[20];
	double x,y;
	pan_PageSize ps;
	SendDlgItemMessage(hDlg,IDC_PS_NAME,WM_GETTEXT,(WPARAM)200,(LPARAM)name);
	SendDlgItemMessage(hDlg,IDC_PS_X,WM_GETTEXT,(WPARAM)20,(LPARAM)strx);
	SendDlgItemMessage(hDlg,IDC_PS_Y,WM_GETTEXT,(WPARAM)20,(LPARAM)stry);
	if (name[0] == 0 || strx[0] == 0 || stry[0] == 0 || !isDouble(strx) || !isDouble(stry))
	{
		MessageBox(hDlg,L"参数错误",L"修改失败",0);
		return 0;
	}
	wcscpy_s(ps.typeName,name);
	swscanf_s(strx,L"%lf",&x);
	swscanf_s(stry,L"%lf",&y);
	ps.a = x;
	ps.b = y;
	unsigned idx;
	idx = SendDlgItemMessage(hDlg,IDC_PS_LIST,LB_GETCURSEL,0,0);
	if(!printContext->modifyPageSize(idx,ps))
	{
		MessageBox(hDlg,L"名字重复",L"修改失败",0);
		return 0;
	}
	showSizeList(hDlg);
	SendDlgItemMessage(hDlg,IDC_PS_LIST,LB_SETCURSEL,(WPARAM)idx,0);
	return 1;
}

void OnListSelChange_ps(HWND hDlg)
{
	unsigned idx;
	pan_PageSize ps;
	wchar_t strx[20],stry[20];
	idx = SendDlgItemMessage(hDlg,IDC_PS_LIST,LB_GETCURSEL,0,0);
	ps = printContext->pageSizes[idx];
	swprintf_s(strx,L"%0.2lf",ps.a);
	swprintf_s(stry,L"%0.2lf",ps.b);
	SendDlgItemMessage(hDlg,IDC_PS_NAME,WM_SETTEXT,0,(LPARAM)ps.typeName);
	SendDlgItemMessage(hDlg,IDC_PS_X,WM_SETTEXT,0,(LPARAM)strx);
	SendDlgItemMessage(hDlg,IDC_PS_Y,WM_SETTEXT,0,(LPARAM)stry);
	if(idx<7)
		EnableWindow(GetDlgItem(hDlg,IDC_PS_MODIFY),FALSE);
	else
		EnableWindow(GetDlgItem(hDlg,IDC_PS_MODIFY),TRUE);

}

void OnRemovePrinter_ps(HWND hDlg)
{
	unsigned idx;
	idx = SendDlgItemMessage(hDlg,IDC_PS_LIST,LB_GETCURSEL,0,0);
	printContext->removePrinter(idx);
	showSizeList(hDlg);
	SendDlgItemMessage(hDlg,IDC_PS_LIST,LB_SETCURSEL,(WPARAM)idx,0);
}
static void OnDlgInit_ps(HWND hDlg)
{
	showSizeList(hDlg);
	SendDlgItemMessage(hDlg,IDC_PS_LIST,LB_SETCURSEL,(WPARAM)0,0);
	OnListSelChange_ps(hDlg);
}
static INT_PTR CALLBACK pan_PrintSettingDlgProc(HWND hDlg,UINT msg, WPARAM wParam,LPARAM lParam )
{
	switch(msg)
	{
	case WM_INITDIALOG:

		OnDlgInit_ps(hDlg);
		return FALSE;


	case WM_COMMAND:
		switch(LOWORD(wParam))
		{
		case IDCANCEL:
			showTypeCombo(GetParent(hDlg));
			showPageList(GetParent(hDlg));
			EndDialog(hDlg, IDCANCEL);
			return TRUE;
		case IDC_PS_SELETE_PRINTER:
			OnSeletePrinter_ps(hDlg);
			break;
		case IDC_PS_REMOVE_PRINTER:
			OnRemovePrinter_ps(hDlg);
			break;
		case IDC_PS_MODIFY:
			OnModify_ps(hDlg);
			break;
		case IDC_PS_DELETE:
			
			break;
		case IDC_PS_ADD:
			OnAdd_ps(hDlg);
			break;
		case IDC_PS_LIST:
			switch(HIWORD(wParam))
			{
			case LBN_SELCHANGE:
				OnListSelChange_ps(hDlg);
				break;
			}
			break;		
		default:
			break;
		}
		break;
	}
	return FALSE;
}

void pan_PrintSettingDlg(HWND hParent)
{
	DialogBox(NULL, MAKEINTRESOURCE(IDD_PRINTER_SETTING), hParent, pan_PrintSettingDlgProc);
}

