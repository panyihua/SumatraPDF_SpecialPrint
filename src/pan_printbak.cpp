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
#include "pan_printSetting.h"
#include <vector>

static HANDLE pan_PrintMutex;
HWND pan_printDlgHwnd;
class pan_PrintContext;
class pan_PageInfo;
static WindowInfo * pan_win;
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

class pan_Printer   //涉及文件的类不是好类
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
	wchar_t sizeName[20];
};
class pan_PageInfo
{
public:
	char type;
	wchar_t * sizeName;
};
class pan_PrintContext
{
	WindowInfo * win;
	DisplayModel *dm;
	HWND hDlg;
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
		pan_PageSize ps;
		ps.a = 0;
		ps.b = 0;
		wcscpy_s(ps.sizeName,_countof(ps.sizeName),L"杂");
		addPageSize(ps);    //有一个默认类型

		addDefaultPageSize();
		generatePageInfo();
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
	void addPageSize(const pan_PageSize& ps)
	{
		pan_PageRange * pageRange;
		pan_Printer * printer;
		pageSizes.Append(ps);

		pageRange = new pan_PageRange(&pageInfos);
		pageRanges.Append(pageRange);

		printer = new pan_Printer();
		printers.Append(printer);
	}
	
	int getPagePrinterNum(int page)
	{
		return pageInfos[page].type;
	}

	int setPagePrinterNum(int type,int page)
	{
		if(type<0 || type >= (int)pageSizes.Size())
			return 0;
		pageInfos[page].type = (char)type;
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
			pi.type = (char)getc(fp);
			pi.sizeName = pageSizes[pi.type].sizeName;
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
			fputc(pageInfos[i].type,fp);
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
			j = pageInfos[i].type;
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
private:

	void addDefaultPageSize()
	{
		pan_PageSize ps;
		ps.a = 29.7;
		ps.b = 42;
		wcscpy_s(ps.sizeName,_countof(ps.sizeName),L"A3");
		addPageSize(ps);
		ps.a = 21;
		ps.b = 29.7;
		wcscpy_s(ps.sizeName,_countof(ps.sizeName),L"A4");
		addPageSize(ps);
		ps.a = 14.8;
		ps.b = 21;
		wcscpy_s(ps.sizeName,_countof(ps.sizeName),L"A5");
		addPageSize(ps);
	}

	int getPageTypeFromPDF(int num)   //range 函数依赖 type
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

	void generatePageInfo()
	{
		int i,j;
		pageInfos.clear();
		pageInfos.resize(dm->PageCount() + 1);
		for (i=1;i<=dm->PageCount();i++ )
		{
			j = getPageTypeFromPDF(i);
			pageInfos[i].type = (char)j;
			pageInfos[i].sizeName = pageSizes[j].sizeName;
		}
	}

	

	void loadPrinters()
	{
		size_t i;
		for (i = 0;i < printers.Size(); i++)
		{
			printers[i]->setFilePath(str::Format(L"E:\\printer%d",i));
			printers[i]->loadPrinterFromFile();
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
static DWORD WINAPI PrinttingControlThread(LPVOID inData)
{
	printContext->isPrinting = 1;
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
	printContext->isPrinting = 0;
	return 0;
}

void static formatListItemString(wchar_t *title,int len,wchar_t* rangeName,wchar_t * printerName)
{
	if(rangeName)
	{
		wcscat_s(title,len,L"                          ");
		wcscat_s(title,len,rangeName);
	}
	if(printerName)
	{
		wcscat_s(title,len,L"                          ");
		wcscat_s(title,len,printerName);
	}
}

void showPageList(HWND hDlg,WindowInfo *win)
{
	int i;
	unsigned int j;
	wchar_t * rangeName;
	wchar_t * printerName;
	WCHAR title[100];
	HWND list = GetDlgItem(hDlg,IDC_LIST2);
	SendDlgItemMessage(hDlg,IDC_LIST2,LB_RESETCONTENT,0,0);
	int pageNum = printContext->getPrintPagesCount();
	for (i = 1; i <= pageNum; i++)          //page与 range、printer是多对一关联。
	{
		rangeName = NULL;
		printerName = NULL;
		j = printContext->getPagePrinterNum(i);
		rangeName = printContext->pageInfos[i].sizeName;
		printerName = printContext->printers[j]->printerName;
		swprintf_s(title,sizeof(title)/sizeof(WCHAR) - 1 ,L"第 %d 页",i);
		formatListItemString(title,_countof(title),rangeName,printerName);

		ListBox_AppendString_NoSort(list,title);
	}

	SendDlgItemMessage(hDlg, IDC_LIST2, LB_SETCURSEL, 0, 0);
	i = printContext->getPagePrinterNum(1);
	SendDlgItemMessage(hDlg, IDC_COMBO1, CB_SETCURSEL, i , 0);
}

void showPrinterCombo(HWND hDlg,WindowInfo *win)
{
	SendDlgItemMessage(hDlg,IDC_COMBO1,CB_RESETCONTENT,0,0);
	unsigned int printerNum = printContext->printers.Size();
	for (unsigned i = 0; i< printerNum;i++ )
	{
		SendDlgItemMessage(hDlg, IDC_COMBO1, CB_INSERTSTRING, (WPARAM)-1, (LPARAM)printContext->printers[i]->printerName);
	}
}
void OnDlgInit(HWND hDlg,WindowInfo *win)
{
	showPrinterCombo(hDlg,win);
	showPageList(hDlg,win);
}

static void OnPrinterAlter(HWND hDlg)
{
	int newPrinterNum,page = 0;
	page = ListBox_GetCurSel(GetDlgItem(hDlg, IDC_LIST2))+1;
	newPrinterNum = SendDlgItemMessage(hDlg, IDC_COMBO1, CB_GETCURSEL, 0, 0);
	printContext->setPagePrinterNum(newPrinterNum,page);


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
void pan_DoPrint(WindowInfo *win);
static void OnOK(HWND hDlg,WindowInfo * win)
{
	pan_DoPrint(win);
}

static void OnSetting(HWND hDlg,WindowInfo * win)
{
	pan_PrintSettingDlg(hDlg);
}
static void OnSavePage(HWND hDlg,WindowInfo * win)
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
	ofn.lpstrFilter = L"页配置文件(*.ppag)\0*.ppag\0所有文件(*.*)\0*.*\0\0";
	if(GetSaveFileName(&ofn) == 0)
		return;
	wcscat_s(filePath,L".ppag");
	printContext->savePageSizeAndPageInfo(filePath);
}
static void OnLoadPage(HWND hDlg,WindowInfo *win)
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
	showPageList(hDlg,win);
}
 static INT_PTR CALLBACK pan_PrintDlgProc(HWND hDlg,UINT msg, WPARAM wParam,LPARAM lParam )
{
	WindowInfo *win = pan_win;
	switch(msg)
	{
	case WM_INITDIALOG:
		printContext = new pan_PrintContext();
		printContext->init(win,hDlg);
		OnDlgInit(hDlg,win);
		return TRUE;
	case WM_CLOSE :
		DestroyWindow(hDlg);
		delete printContext;
		pan_printDlgHwnd = NULL;
		break;

	case WM_COMMAND:
		switch(LOWORD(wParam))
		{
		case IDOK:
			OnOK(hDlg,win);
			DestroyWindow(hDlg);
			delete printContext;
			pan_printDlgHwnd = NULL;
			break;
		case IDCANCEL:
			DestroyWindow(hDlg);
			delete printContext;
			pan_printDlgHwnd = NULL;
			break;
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
		case IDC_LOAD_PAGE:
			OnLoadPage(hDlg,win);
			break;
		case IDC_SAVE_PAGE:
			OnSavePage(hDlg,win);
			break;
			
		default:
			
			break;
		}
		return TRUE;
	}
	return FALSE;
}

void pan_PrintDlg(WindowInfo *win)
{
	pan_win = win;
	pan_printDlgHwnd = CreateDialog(NULL,MAKEINTRESOURCE(IDD_PRINT),win->hwndFrame,pan_PrintDlgProc);
	ShowWindow(pan_printDlgHwnd,SW_SHOW);
	
}

PrintData * pan_CreatePrintData(pan_Printer * printer,pan_PageRange *pageRange,WindowInfo *win)
{
	Print_Advanced_Data advanced(PrintRangeAll, PrintScaleShrink, false);
	PrintData *data = new PrintData(win->dm->engine, &(printer->printerInfo), printer->pDevMode, 
		pageRange->ppr, advanced, win->dm->Rotation(), NULL);
	return data;
}
void pan_DoPrint(WindowInfo *win)
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
}
