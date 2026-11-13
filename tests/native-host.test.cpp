// DirectWrite/Win32 regression coverage of the production host implementation.
// No player connection or hotkey. Focused input checks use an ephemeral
// loopback listener without a client; production-procedure windows stay offscreen.
#include "../native/Host.cpp"
#include <iostream>
#include <stdexcept>
#include <cstring>

struct NativeTest {
 static int checks;
 static void require(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
 static void expectNear(double a,double b,const char* message){require(std::abs(a-b)<.03,message);}
 struct Fixture {
  App a;
  Fixture(){
   a.background.mode=0; // Lyric-only scenarios do not request background animation frames.
   check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(a.textFactory.put())));
   a.makeFormat(a.mainFormat,CurrentFontSize,DWRITE_FONT_WEIGHT_SEMI_BOLD);a.makeFormat(a.secondaryFormat,TranslationFontSize,DWRITE_FONT_WEIGHT_NORMAL);
   a.mainRowHeight=a.layout(L"Ag",a.mainFormat.get()).height;a.updatePlaceholders();
   a.width=600;a.height=240;a.viewportBottom=212;a.freezePlayback=true;a.testWall=true;a.testWallMs=1000;
   a.motion.setRunning(true,0);a.coverSince=-1000;
   a.hwnd=CreateWindowExW(0,L"STATIC",L"MiniLyrics isolated test",WS_POPUP,0,0,600,240,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
   require(a.hwnd!=nullptr,"create hidden test window");
   a.renderVisible=true; // Explicitly exercise drawing/scheduling in offscreen fixtures.
   a.transport=transportGeometry(a.width,a.height,a.textX,1);
   a.frameTimer=CreateWaitableTimerW(nullptr,FALSE,nullptr);require(a.frameTimer!=nullptr,"create frame timer");
  }
  ~Fixture(){a.modalFrames.stop();if(a.frameTimer){CancelWaitableTimer(a.frameTimer);CloseHandle(a.frameTimer);a.frameTimer=nullptr;}if(a.hwnd){DestroyWindow(a.hwnd);a.hwnd=nullptr;}}
  void lines(int mode=0){
   a.lines.clear();a.groups.clear();a.focusIndex=-999;
   for(int i=0;i<40;i++){Line l;l.time=i*2000;l.duration=2000;l.text=L"Lyric "+std::to_wstring(i);if(mode==1||(mode==2&&i%2==0))l.translation=L"Translation";a.lines.push_back(std::move(l));}
   a.index=10;a.position=21000;a.valid=true;
  }
  void layout(int focus,int browse=-1){a.browseIndex=browse;a.reflow(focus,false);a.updateIndicators();a.transitionActive=false;}
  void wheel(int delta){POINT p{int(a.textX+10),int((a.viewportTop+a.viewportBottom)*.5f)};ClientToScreen(a.hwnd,&p);a.message(WM_MOUSEWHEEL,MAKEWPARAM(0,SHORT(delta)),MAKELPARAM(p.x,p.y));}
  UINT interval(double position){
   a.position=position;a.playing=true;a.valid=true;
   CancelWaitableTimer(a.frameTimer);a.frameArmed=false;a.frameRunning=false;a.frameDeadline=0;a.schedule();return a.intervalMs;
  }
 };
 static void typography(){
  for(int mode:{0,1,2}){
   Fixture f;auto&a=f.a;f.lines(mode);
   for(int focus:{9,10,11,10}){
    f.layout(focus,focus);expectNear(a.diagnostics("test")["groupGap"].n(),7,"gap must not depend on translation");
    for(int at=focus-3;at<=focus+3;at++){
     const auto&g=a.group(at);float scale=g.zoom.value(a.motionNow());
     expectNear(CurrentFontSize*scale,at==focus?21:16,"focus/context original font");
     float expected=g.mainHeight*scale;
     if(g.translated){FLOAT size=0;check(g.subtitle.value->GetFontSize(0,&size,nullptr));expectNear(size,16,"translation layout remains 16");expected+=(6.3f+g.subtitle.height)*scale;expectNear(g.subtitleOffset(scale)-g.mainHeight*scale,6.3*scale,"within-group gap follows group scale");}
     expectNear(g.renderedHeight(scale),expected,"rendered group height includes scaled translation");
     if(at<focus+3){const auto&next=a.group(at+1);expectNear(next.y.value(a.motionNow())-g.y.value(a.motionNow())-g.renderedHeight(scale),7,"adjacent groups must have exact 7 DIP gap");}
    }
    auto built=a.lineBuildCalls,id=a.group(focus).identity;
    for(float height:{130.f,192.f,240.f}){a.height=height;a.reflow(focus,false);require(a.lineBuildCalls==built,"resize must reuse text layouts");require(a.group(focus).identity==id,"resize must retain group identity");}
   }
   a.height=240;f.layout(10);a.position=21000;a.playing=true;
   a.reflow(11,true);a.testWallMs+=350;float mid=a.group(11).zoom.value(a.motionNow());require(mid>16.f/21.f&&mid<1,"promotion scales original smoothly");
   if(a.group(11).translated)expectNear(a.group(11).renderedHeight(mid)-a.group(11).mainHeight*mid,(6.3+a.group(11).subtitle.height)*mid,"translation scales during promotion");
   double paused=a.motionNow();a.motion.setRunning(false,a.testWallMs);a.testWallMs+=1000;expectNear(a.motionNow(),paused,"pause freezes the size/position clock");
   require(a.groups.size()<=32,"bounded layout cache");
  }
  // A small translated group can end inside the viewport even when its main text is above it.
  Fixture f;f.lines(2);f.layout(11,11);auto&g=f.a.group(10);float scale=g.zoom.value(f.a.motionNow());
  g.y.snap(f.a.viewportTop-g.renderedHeight(scale)+1);require(f.a.groupVisible(g,f.a.motionNow()),"scaled subtitle contributes to visible bounds");
  g.y.snap(f.a.viewportTop-g.renderedHeight(scale)-1);require(!f.a.groupVisible(g,f.a.motionNow()),"whole group outside viewport is excluded");
 }
 static void motionGeometry(){
  for(int mode:{0,1,2})for(int curve:{0,1,2,3})for(bool browse:{false,true})for(int direction:{-1,1}){
   Fixture f;auto&a=f.a;f.lines(mode);f.layout(10);a.lineCurve=curve;a.playing=true;a.browseIndex=browse?10+direction:-1;a.reflow(10+direction,true);
   for(int t=0;t<=800;t+=25){a.testWallMs=1000+t;double now=a.motionNow();
    for(int at=8;at<13;at++){const auto&g=a.group(at);const auto&next=a.group(at+1);float bottom=g.y.value(now)+g.renderedHeight(g.zoom.value(now));require(bottom<=next.y.value(now)+.03,"adjacent original/translation groups must not overlap in motion");}
   }
  }
 }
 static void scheduling(){
  Fixture f;auto&a=f.a;f.lines();f.layout(4,4);
  for(bool stagger:{false,true})for(int fps:{30,60}){
   a.stagger=stagger;a.fps=fps;
   for(double p:{21800.,21850.,21900.,21990.})expectNear(f.interval(p),22000-p,"manual browse must not repeatedly wake for past auto-lookahead");
  }
  a.stagger=true;a.fps=30;f.layout(10);expectNear(f.interval(21750),50,"auto-follow still wakes at the 200ms lookahead boundary");
  f.layout(11);expectNear(f.interval(21850),150,"auto-follow waits for actual next lyric once lookahead completes");
  a.lines[10].words.push_back({L"word",20000,2000});f.layout(4,4);
  require(!a.groupVisible(a.group(10),a.motionNow()),"playback word is offscreen in this fixture");expectNear(f.interval(21000),200,"offscreen word does not drive high-frequency frames");
  f.layout(10,10);expectNear(f.interval(21000),34,"visible playback word still animates during browse");a.fps=60;expectNear(f.interval(21000),17,"visible playback word retains 60 FPS");
  a.fps=30;a.lines[10].words.clear();a.lines[10].text=std::wstring(150,L'W');f.layout(4,4);
  require(!a.groupVisible(a.group(10),a.motionNow()),"long playback lyric is offscreen");expectNear(f.interval(21000),200,"offscreen long lyric does not drive frames");
  f.layout(10,10);expectNear(f.interval(21000),200,"visible long original remains fixed while browsing");
  f.layout(10);expectNear(f.interval(21000),34,"visible long original scrolls during auto-follow");
  a.lines[10].text=L"Short";a.lines[10].translation=std::wstring(150,L'W');f.layout(10);
  expectNear(f.interval(21000),34,"visible long translation still scrolls");f.layout(10,10);expectNear(f.interval(21000),200,"translation scrolling also stops while browsing");
  a.lines[10].translation.clear();a.lines[11].text=std::wstring(150,L'W');f.layout(10);expectNear(f.interval(21000),200,"long future preview stays fixed without high-frequency refresh");
  f.layout(4,4);a.transitionActive=true;expectNear(f.interval(21000),34,"browse motion still receives animation frames");a.transitionActive=false;
  a.coverSince=nowMs();expectNear(f.interval(21000),34,"cover animation remains independent of lyric visibility");a.coverSince=-1000;
  a.browseUntil=a.testWallMs+4000;a.testWallMs+=4001;a.update();require(a.browseIndex<0&&a.focusIndex==10,"four-second return remains active");require(a.index==10,"browsing and return do not seek playback");
  a.playing=false;a.update();require(!a.frameRunning,"pause stops frame scheduling");
 }
 struct Surface {
  static constexpr int width=600,height=160;
  HDC dc=nullptr;HBITMAP bitmap=nullptr;HGDIOBJ old=nullptr;BYTE*pixels=nullptr;
  Com<ID2D1Factory> factory;Com<ID2D1DCRenderTarget> target;Graphics g;
  Surface(){
   check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.put()));
   auto props=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),96,96);
   check(factory->CreateDCRenderTarget(&props,target.put()));dc=CreateCompatibleDC(nullptr);require(dc!=nullptr,"create memory DC");
   BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
   bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels),nullptr,0);require(bitmap!=nullptr,"create memory bitmap");old=SelectObject(dc,bitmap);
   RECT rect{0,0,width,height};check(target->BindDC(dc,&rect));g.target=target.get();check(target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),g.brush.put()));
  }
  ~Surface(){g.clear();target.reset();if(old)SelectObject(dc,old);if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);}
  void save(const std::filesystem::path&file){
   BITMAPFILEHEADER head{};head.bfType=0x4d42;head.bfOffBits=sizeof(head)+sizeof(BITMAPINFOHEADER);head.bfSize=head.bfOffBits+width*height*4;
   BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=width;info.biHeight=-height;info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;
   std::ofstream out(file,std::ios::binary);out.write(reinterpret_cast<char*>(&head),sizeof(head));out.write(reinterpret_cast<char*>(&info),sizeof(info));out.write(reinterpret_cast<char*>(pixels),width*height*4);require(bool(out),"write pixel artifact");
  }
 };
 static void pixels(const std::filesystem::path&directory){
  Fixture f;auto&a=f.a;f.lines(1);f.layout(10,10);a.textX=20;auto&row=a.group(10);row.subtitle=a.layout(L"Translation scales with the lyric",a.secondaryFormat.get());Surface surface;
  std::vector<BYTE> baseline;int baselineWidth=0,baselineHeight=0;
  for(float scale:{1.f,16.f/21.f}){
   row.zoom.snap(scale);row.y.snap(100-row.subtitleOffset(scale));
   surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));a.drawGroup(surface.g,row,10,21000);check(surface.target->EndDraw());GdiFlush();
   std::vector<BYTE> crop(surface.pixels+100*Surface::width*4,surface.pixels+140*Surface::width*4);
   bool ink=false;for(size_t i=0;i<crop.size();i+=4)ink=ink||crop[i]||crop[i+1]||crop[i+2];require(ink,"pixel comparison must contain rendered translation");
   int minX=Surface::width,maxX=-1,minY=40,maxY=-1;
   for(int y=0;y<40;y++)for(int x=0;x<Surface::width;x++){size_t p=(y*Surface::width+x)*4;if(crop[p]||crop[p+1]||crop[p+2]){minX=std::min(minX,x);maxX=std::max(maxX,x);minY=std::min(minY,y);maxY=std::max(maxY,y);}}
   if(baseline.empty()){baseline=crop;baselineWidth=maxX-minX+1;baselineHeight=maxY-minY+1;}
   else {require(crop!=baseline,"context translation must visibly shrink");require(maxX-minX+1<baselineWidth*.85,"translation pixel width follows group scale");require(maxY-minY+1<baselineHeight,"translation pixel height follows group scale");}
   if(!directory.empty())surface.save(directory/(scale==1?"original-21-translation-16.bmp":"original-16-translation-12.bmp"));
  }
 }
 static void interludes(const std::filesystem::path&directory){
  // Match RNP Interlude's floor(duration/3), clamp, opacity and scale formulas.
  for(int i=0;i<3;i++)for(int elapsed:{-100,0,1000,2000,3000,4000,5000,6000,7000}){
   auto dot=interludeDot(10000+elapsed,10000,6000,i,true);double p=unit((elapsed-i*2000)/2000.);
   expectNear(dot.alpha,.2+.7*p,"RNP per-dot progress controls opacity");expectNear(dot.scale,.9+.1*p,"RNP per-dot progress controls scale");
   auto inactive=interludeDot(10000+elapsed,10000,6000,i,false);expectNear(inactive.alpha,.2,"inactive dots reset to RNP base opacity");expectNear(inactive.scale,.9,"inactive dots reset to RNP base scale");
  }
  expectNear(interludeDot(3000,0,6002,1,true).alpha,.55,"RNP divides duration into integer millisecond thirds");
  require(std::isfinite(interludeDot(1,0,0,2,true).alpha),"zero-duration interlude cannot divide by zero");
  InterludeAnimation visual;visual.update(true,true,0);expectNear(visual.opacity.value(499),0,"RNP waits 500ms before fading in");require(visual.opacity.value(750)>0&&visual.opacity.value(750)<1,"RNP fades in over the following 500ms");expectNear(visual.opacity.value(1000),1,"RNP interlude is fully visible after one second");
  expectNear(visual.scale(0),1,"RNP breath begins at normal scale");expectNear(visual.scale(1000),1.1,"RNP breath reaches 1.1 at one second");expectNear(visual.scale(2000),1,"RNP breath repeats every two seconds");
  visual.update(false,true,2000,6000,0,6000);expectNear(visual.dot(6000,0,6000,0,2000).alpha,.9,"RNP exit starts from the previous dot opacity");auto exiting=visual.dot(6000,0,6000,0,2100);require(exiting.alpha>.2&&exiting.alpha<.9&&exiting.scale>.9&&exiting.scale<1,"RNP dots reset smoothly over 200ms");expectNear(visual.dot(6000,0,6000,0,2200).alpha,.2,"RNP dot reset completes after 200ms");
  Fixture f;auto&a=f.a;a.width=600;a.height=160;a.viewportBottom=132;a.textX=20;a.playing=true;a.valid=true;a.position=1000;a.fps=30;
  Line first;first.time=6000;first.duration=5000;first.text=L"First lyric";a.lines={first};a.index=a.rowAt(1000);a.updatePlaceholders();f.layout(0);
  require(a.lyricView()==App::LyricView::Prelude,"confirmed time before first lyric selects prelude");
  require(a.groupVisible(a.group(0),a.motionNow()),"prelude reuses its real preview group for drawing and hit testing");
  f.interval(1000);expectNear(a.framePeriod,1000./30,"prelude follows lyric animation frame rate");a.fps=60;f.interval(1000);expectNear(a.framePeriod,1000./60,"prelude supports the 60 FPS setting");a.fps=30;
  a.testWallMs+=1000;
  Surface surface;
  auto render=[&](){surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));a.drawLyrics(surface.g,a.current()+a.offset);check(surface.target->EndDraw());GdiFlush();return std::vector<BYTE>(surface.pixels,surface.pixels+Surface::width*Surface::height*4);};
  auto indicatorCrop=[&](const std::vector<BYTE>&frame){int top=int(std::floor(a.lyricAnchor())),bottom=int(std::ceil(a.lyricAnchor()+a.mainRowHeight));return std::vector<BYTE>(frame.begin()+top*Surface::width*4,frame.begin()+bottom*Surface::width*4);};
  auto prelude=indicatorCrop(render());bool ink=false;for(size_t i=0;i<prelude.size();i+=4)ink=ink||prelude[i]||prelude[i+1]||prelude[i+2];require(ink,"prelude pixel sample contains indicator");
  if(!directory.empty())surface.save(directory/"unified-prelude.bmp");
  Line gap;gap.time=0;gap.duration=6000;gap.isInterlude=true;a.lines.insert(a.lines.begin(),gap);a.groups.clear();a.focusIndex=-999;a.index=a.rowAt(1000);a.updatePlaceholders();f.layout(0);a.testWallMs+=1000;
  require(a.lines.size()==2&&a.rowAt(5999)==0&&a.rowAt(6000)==1,"empty timestamp is retained for exact lyric switching");
  require(a.lyricView()==App::LyricView::Rows&&a.showInterlude(a.group(0)),"leading empty timed row uses shared indicator");
  require(indicatorCrop(render())==prelude,"prelude and timed empty row render identical indicator pixels");
  if(!directory.empty())surface.save(directory/"unified-timed-gap.bmp");
  f.interval(1000);expectNear(a.framePeriod,1000./30,"timed gap has the same indicator frame rate");
  auto early=indicatorCrop(render());a.position=5000;a.snapRow=true;a.update();auto late=indicatorCrop(render());require(late!=early,"seek changes actual dot pixels without restarting the fade");expectNear(a.group(0).dots.opacity.value(a.motionNow()),1,"seek within current interlude preserves container visibility");a.position=1000;a.snapRow=true;a.update();require(indicatorCrop(render())==early,"backward seek immediately restores dot progress");
  a.motion.setRunning(false,a.testWallMs);a.playing=false;a.updateIndicators();auto paused=render();a.testWallMs+=500;a.schedule();require(!a.frameRunning,"pause stops indicator timer");require(render()==paused,"paused indicator remains pixel-identical");
  a.motion.setRunning(true,a.testWallMs);a.playing=true;a.updateIndicators();a.testWallMs+=500;require(indicatorCrop(render())!=indicatorCrop(paused),"resumed indicator continues breathing");
  f.layout(0,0);require(!a.showInterlude(a.group(0))&&!a.groupVisible(a.group(0),a.motionNow()),"browsed empty row has no indicator or hit target");f.interval(1000);expectNear(a.framePeriod,200,"browsed empty row does not force animation frames");
  a.valid=false;require(a.lyricView()==App::LyricView::Rows,"manual browse remains available while clock is unconfirmed");
  f.layout(0);require(a.lyricView()==App::LyricView::Syncing,"unconfirmed progress uses syncing state");require(!a.groupVisible(a.group(1),a.motionNow())&&!a.showInterlude(a.group(0)),"unconfirmed progress draws no timed indicator or rows");
  a.valid=true;a.index=0;f.layout(0);a.testWallMs+=1000;a.index=1;f.layout(1);require(a.indicatorVisible(a.group(0)),"ending interlude fades out for 500ms like RNP");a.testWallMs+=500;require(!a.groupVisible(a.group(0),a.motionNow()),"past empty row disappears when its fade completes");f.interval(6500);expectNear(a.framePeriod,200,"past empty row does not drive high frequency redraw after fade");
  a.index=0;f.layout(0);a.group(0).y.snap(a.viewportBottom+1);f.interval(1000);expectNear(a.framePeriod,200,"offscreen active gap does not drive high frequency redraw");
  a.lines[0].words.push_back({L"Word-only lyric",0,5000});require(!a.emptyGroup(a.group(0))&&a.isInterlude(a.group(0)),"explicit RNP interlude marker survives word data");
  a.lines[0].words.clear();a.lines[0].translation=L"Translation-only lyric";require(a.group(0).translated&&a.isInterlude(a.group(0)),"RNP marker survives translated content");
  a.lines[0].duration=0;expectNear(a.interludeDuration(a.lines[0],0),6000,"missing interlude duration uses the next timestamp");a.lines[1].duration=0;a.duration=12000;expectNear(a.interludeDuration(a.lines[1],1),6000,"last row duration can use track end");Line orphan;orphan.time=2000;expectNear(a.interludeDuration(orphan,100),10000,"retained fading row remains safe after lyric list shrinks");
  a.lines.erase(a.lines.begin());a.groups.clear();a.index=a.rowAt(-1000);a.position=0;a.offset=-1000;a.updatePlaceholders();require(a.lyricView()==App::LyricView::Prelude,"negative lyric offset uses the same prelude state");
  a.lines.clear();a.valid=false;a.updatePlaceholders();require(a.lyricView()==App::LyricView::Waiting,"missing lyrics remain a separate waiting state");
  Fixture wire;Json message=Json::obj(),raw=Json::obj();message["type"]="lyrics";message["songId"]="";message["lines"].kind=Json::Array;raw["time"]=0;raw["duration"]=6000;raw["text"]="Marked";raw["isInterlude"]=true;message["lines"].array.push_back(raw);wire.a.receive(message);
  require(wire.a.lines[0].isInterlude&&wire.a.group(0).model.isInterlude,"native bridge preserves explicit RNP marker");message["lines"].array[0]["isInterlude"]=false;wire.a.receive(message);require(!wire.a.lines[0].isInterlude&&!wire.a.group(0).model.isInterlude,"marker-only change updates native cached row");
 }
 static void browseInterludes(const std::filesystem::path&directory){
  auto prepare=[](Fixture&f){auto&a=f.a;a.lines.clear();a.groups.clear();a.focusIndex=-999;
   for(int i=0;i<8;i++){Line line;line.time=i*1000;line.duration=1000;line.isInterlude=i!=1&&i!=4&&i!=6;if(!line.isInterlude)line.text=L"Lyric "+std::to_wstring(i);if(i==1)line.translation=L"Translation";if(i==3)line.isInterlude=false;a.lines.push_back(line);}
   a.position=1500;a.index=a.rowAt(a.position);a.valid=true;a.playing=false;a.updatePlaceholders();f.layout(1);
  };
  Fixture f;prepare(f);auto&a=f.a;f.wheel(-120);require(a.browseIndex==2,"one wheel notch reaches the middle interlude placeholder");require(a.index==1&&a.position==1500,"browsing does not seek or change the playback row");
  for(int i=1;i<6;i++){const auto&g=a.group(i);const auto&next=a.group(i+1);expectNear(next.y.value(a.motionNow())-g.y.value(a.motionNow())-g.renderedHeight(g.zoom.value(a.motionNow())),7,"middle placeholders retain row height and exact 7 DIP gaps");}
  require(a.staticInterlude(a.group(2))&&a.groupVisible(a.group(2),a.motionNow()),"focused middle interlude is a visible static placeholder");require(a.staticInterlude(a.group(3))&&a.groupVisible(a.group(3),a.motionNow()),"adjacent middle interlude also remains visible");require(!a.groupVisible(a.group(0),a.motionNow()),"leading interlude remains hidden during browse");
  f.wheel(-120);require(a.browseIndex==3,"consecutive middle interludes retain separate placeholder rows");f.wheel(-120);require(a.browseIndex==4,"next notch reaches the following lyric");f.wheel(-240);require(a.browseIndex==6,"multi-notch wheel includes the middle placeholder");f.wheel(-120);require(a.browseIndex==6,"trailing interlude cannot become browse focus");
  f.wheel(120);require(a.browseIndex==5,"reverse wheel also reaches the middle placeholder");f.wheel(1200);require(a.browseIndex==1,"leading interludes remain outside browse bounds");
  f.wheel(-60);require(a.browseIndex==1,"partial wheel delta does not consume a row");f.wheel(-60);require(a.browseIndex==2,"accumulated partial deltas reach a middle placeholder");
  a.lines[2].isInterlude=false;a.lines[2].text=L"New lyric";a.layoutDirty=true;a.update();require(a.browseIndex==2&&!a.staticInterlude(a.group(2)),"lyric update replaces placeholder content without moving browse focus");
  a.lines[2].isInterlude=true;a.lines[2].text.clear();a.layoutDirty=true;a.update();require(a.browseIndex==2&&a.staticInterlude(a.group(2)),"lyric update can restore the middle placeholder in place");
  a.testWallMs+=4001;a.update();require(a.browseIndex<0&&a.index==1&&a.focusIndex==1,"four-second return restores original playback row");
  for(int delta:{-120,120}){Fixture gap;prepare(gap);auto&g=gap.a;g.position=2500;g.index=g.rowAt(g.position);gap.layout(2);gap.wheel(delta);require(g.browseIndex==(delta<0?3:1),"browsing from current interlude moves one row including placeholders");g.testWallMs+=4001;g.update();require(g.browseIndex<0&&g.index==2&&g.focusIndex==2,"returning to playback retains the timed interlude");require(g.lines[2].time==2000&&g.rowAt(2500)==2,"browsing never removes an interlude timestamp");require(!g.staticInterlude(g.group(2)),"returning to playback removes static placeholder mode");}
  for(int delta:{-120,120}){Fixture intro;auto&g=intro.a;Line first;first.time=5000;first.text=L"First";Line second;second.time=8000;second.text=L"Second";g.lines={first,second};g.index=-1;g.valid=true;intro.layout(0);intro.wheel(delta);require(g.browseIndex==0,"first wheel gesture before lyrics reaches first real lyric");}
  Fixture empty;auto&e=empty.a;Line blank;blank.isInterlude=true;blank.duration=1000;e.lines={blank,blank};e.lines[1].time=1000;e.valid=true;e.index=0;empty.layout(0);empty.wheel(-120);require(e.browseIndex<0,"all-interlude data does not enter an invisible browse mode");
  Fixture single;auto&s=single.a;Line only;only.time=1000;only.text=L"Only lyric";s.lines={blank,only,blank};s.lines[2].time=2000;s.valid=true;s.index=0;single.layout(0);single.wheel(-120);require(s.browseIndex==1,"single real lyric can be reached through leading blanks");single.wheel(120);single.wheel(-120);require(s.browseIndex==1,"single real lyric remains the boundary in both directions");
  Fixture visual;prepare(visual);auto&v=visual.a;visual.wheel(-120);v.height=160;v.viewportBottom=132;v.textX=20;visual.layout(2,2);v.playing=true;v.motion.setRunning(true,v.testWallMs);v.updateIndicators();Surface surface;
  auto render=[&](){surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));v.drawGroup(surface.g,v.group(2),2,v.current()+v.offset);check(surface.target->EndDraw());GdiFlush();return std::vector<BYTE>(surface.pixels,surface.pixels+Surface::width*Surface::height*4);};
  auto stationary=render();bool ink=false;for(size_t i=0;i<stationary.size();i+=4)ink=ink||stationary[i]||stationary[i+1]||stationary[i+2];require(ink,"middle placeholder draws actual dot pixels");if(!directory.empty())surface.save(directory/"static-middle-placeholder.bmp");
  v.testWallMs+=1000;v.position=2500;v.index=2;v.roles();require(render()==stationary,"placeholder pixels remain identical when time, progress and active row change");v.finishTransition();visual.interval(2500);expectNear(v.framePeriod,200,"static placeholder does not request animation frames");
  v.valid=false;require(v.groupVisible(v.group(2),v.motionNow())&&render()==stationary,"static placeholder remains visible without confirmed progress during browse");
  v.valid=true;v.stopBrowsing();v.update();require(!v.staticInterlude(v.group(2))&&v.showInterlude(v.group(2)),"returning to playback restores RNP interlude mode");v.testWallMs+=1000;require(render()!=stationary,"playback interlude resumes timed dot animation");
  Fixture longGap;auto&l=longGap.a;for(int i=0;i<1002;i++){Line line;line.time=i*1000;line.duration=1000;line.isInterlude=i>0&&i<1001;if(!line.isInterlude)line.text=L"Visible lyric";l.lines.push_back(line);}l.position=500;l.index=0;l.valid=true;l.playing=true;longGap.layout(0);longGap.wheel(-120);require(l.browseIndex==1,"long middle gap is browsed through placeholder rows");const auto&first=l.group(0);const auto&last=l.group(1);require(l.groups.size()<=32&&l.lineBuildCalls<32,"long interlude run keeps text cache and layout construction bounded");
  require(l.transitionActive&&last.y.active(l.motionNow()),"moving onto a placeholder retains normal browse animation");for(int t=0;t<=250;t+=25){l.testWallMs=1000+t;expectNear(last.y.value(l.motionNow())-first.y.value(l.motionNow())-first.renderedHeight(first.zoom.value(l.motionNow())),7,"placeholder rows retain exact spacing throughout browse animation");}
 }
 static void fontSizes(const std::filesystem::path&directory){
  Fixture f;auto&a=f.a;f.lines(2);a.playing=false;f.layout(10);
  a.settingsPath=(directory/L"font-settings.json").wstring();
  auto verify=[&](int size){a.setLyricFontSize(size);FLOAT mainSize=0,subSize=0;
   check(a.group(10).main.value->GetFontSize(0,&mainSize));check(a.group(10).subtitle.value->GetFontSize(0,&subSize));
   expectNear(mainSize,size,"actual lyric layout font size");expectNear(subSize,16.f*size/21,"translation layout scales proportionally");
   require(a.flight.rows.empty()&&a.group(10).placed,"font change rebuilds positioned text without stale flight");
  };
  verify(14);float smallWidth=a.group(10).main.width;verify(36);require(a.group(10).main.width>smallWidth,"larger font changes measured text width");verify(28);
  auto oldSettings=env(L"MINILYRICS_SETTINGS");SetEnvironmentVariableW(L"MINILYRICS_SETTINGS",a.settingsPath.c_str());
  try{App restored;expectNear(restored.currentFontSize,28,"font size persists in native settings");expectNear(restored.translationFontSize,16.f*28/21,"restored translation scale");}catch(...){SetEnvironmentVariableW(L"MINILYRICS_SETTINGS",oldSettings.empty()?nullptr:oldSettings.c_str());throw;}
  SetEnvironmentVariableW(L"MINILYRICS_SETTINGS",oldSettings.empty()?nullptr:oldSettings.c_str());
  a.setLyricFontSize(99);expectNear(a.currentFontSize,36,"native upper size bound");a.setLyricFontSize(1);expectNear(a.currentFontSize,14,"native lower size bound");verify(21);
  std::cout<<"PASS: font size, actual DirectWrite layouts, bounds and persistence\n";
 }
 static void seekFlights(const std::filesystem::path&directory){
  {
   Fixture external;auto&x=external.a;external.lines(2);x.songId="external-test";x.playing=true;external.layout(10);
   Json seek=Json::obj();seek["type"]="seek";seek["songId"]="stale-track";seek["position"]=41000;x.receive(seek);
   require(x.flight.rows.empty()&&x.position==21000,"stale track seek ignored");seek["songId"]=x.songId;x.receive(seek);
   require(!x.flight.rows.empty()&&x.index==20,"external seek uses the existing flight entry");
   double started=x.flight.started;Json echo=Json::obj();echo["type"]="clock";echo["position"]=41000;echo["playing"]=true;echo["valid"]=true;x.receive(echo);
   require(x.flight.started==started,"external seek timeline echo does not restart flight");
   float from=x.flight.rows[20].point.y;x.testWallMs+=300;x.update();require(x.flight.rows[20].point.y<from,"external seek has intermediate geometry");
   float current=x.flight.rows[20].point.y;seek["position"]=11000;x.receive(seek);
   expectNear(x.flight.rows[20].point.y,current,"external rapid retarget starts at displayed frame");
   x.testWallMs+=1000;x.update();require(x.flight.rows.empty()&&x.index==5,"external backward seek completes");
   x.playing=false;x.update();seek["position"]=41000;x.receive(seek);require(x.frameRunning,"paused external seek schedules flight");
   x.testWallMs+=1000;x.update();require(x.flight.rows.empty()&&!x.playing&&!x.frameRunning,"paused external seek finishes without resuming");
   Json correction=Json::obj();correction["type"]="clock";correction["position"]=61000;correction["playing"]=false;correction["valid"]=true;x.receive(correction);
   require(x.flight.rows.empty()&&x.index==30,"clock-only correction does not masquerade as a seek");
  }
  Fixture f;auto&a=f.a;f.lines(2);a.height=160;a.viewportBottom=132;a.textX=20;a.playing=true;f.layout(10);Surface surface;surface.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  auto render=[&](){surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));surface.target->PushAxisAlignedClip(D2D1::RectF(a.textX,a.viewportTop,a.width-12,a.viewportBottom),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);a.drawLyrics(surface.g,a.current()+a.offset);surface.target->PopAxisAlignedClip();check(surface.target->EndDraw());GdiFlush();return std::vector<BYTE>(surface.pixels,surface.pixels+Surface::width*Surface::height*4);};
  auto before=render();auto identity=a.group(10).identity;a.seekTo(41000);a.update();require(a.index==20&&!a.flight.rows.empty(),"seek updates progress immediately and starts a flight");require(a.group(10).identity==identity,"flight retains source layouts at its first frame");require(render()==before,"flight first frame matches the previous lyric pixels");
  float start=a.flight.rows[20].point.y;a.testWallMs=1300;a.update();bool intermediate=false;for(const auto&item:a.groups)if(item.first>10&&item.first<20&&a.groupVisible(item.second,a.motionNow()))intermediate=true;require(intermediate,"intermediate lyrics cross the viewport during a long seek");require(a.flight.rows[20].point.y<start&&a.flight.rows[20].point.y>a.lyricAnchor(),"target advances continuously towards its anchor");auto middle=render();require(middle!=before,"flight changes actual rendered pixels");require(a.groups.size()<=32,"flight preserves bounded text cache");if(!directory.empty())surface.save(directory/"seek-flight-middle.bmp");
  a.testWallMs=1900;a.update();require(a.flight.rows.empty(),"flight completes after 500ms plus stagger");expectNear(a.group(20).y.value(a.motionNow()),a.lyricAnchor(),"target lands exactly at lyric anchor");require(a.position==41000&&render()!=middle,"visual flight does not delay or change playback position");a.seekTo(41850);a.update();require(a.flight.focus==21&&a.flight.playbackIndex==20,"lookahead focus can precede actual playback row");a.position=42010;a.update();require(a.flight.playbackIndex==21&&a.flight.rows[21].to.mainAlpha==1&&!a.flight.stagger,"new playback row updates highlight without repeating the stagger");std::cout<<"PASS: forward seek and intermediate lyric pixels\n";
  a.seekTo(61000);a.update();a.testWallMs=2200;a.update();auto retargetFrame=render();a.seekTo(11000);a.update();require(render()==retargetFrame,"rapid backward seek continues from the currently displayed frame");double started=a.flight.started;Json clock=Json::obj();clock["type"]="clock";clock["position"]=11000;clock["playing"]=true;clock["valid"]=true;clock["seekState"]="pending";a.receive(clock);require(!a.flight.rows.empty()&&a.flight.started==started,"pending seek echo cannot reset flight");clock["seekState"]="confirmed";a.receive(clock);require(a.flight.started==started,"seek confirmation cannot restart flight");a.testWallMs=3100;a.update();require(a.flight.rows.empty()&&a.index==5,"rapid backward seek lands on latest target");std::cout<<"PASS: rapid retarget and pending/confirmed clock echoes\n";
  Fixture paused;auto&p=paused.a;paused.lines(2);paused.layout(10);p.playing=false;p.seekTo(41000);p.update();double frozen=p.motionNow();require(p.frameRunning,"paused seek temporarily schedules animation frames");float pausedStart=p.flight.rows[20].point.y;p.testWallMs=1300;p.update();require(p.motionNow()==frozen&&p.flight.rows[20].point.y<pausedStart,"paused playback clock stays frozen while flight advances");p.testWallMs=1900;p.update();require(p.flight.rows.empty()&&!p.frameRunning&&!p.playing&&p.position==41000,"paused seek finishes and returns to sleep without starting playback");
  for(auto&line:p.lines)line.time+=5000;p.groups.clear();p.focusIndex=-999;p.position=46000;p.index=p.rowAt(p.position);paused.layout(p.index);p.updatePlaceholders();p.seekTo(0);p.update();p.testWallMs=2800;p.update();require(p.flight.rows.empty()&&p.lyricView()==App::LyricView::Prelude,"seek before first lyric lands in prelude state");expectNear(p.preludeDots.opacity.value(p.motionNow()),1,"paused prelude landing keeps frozen dots visible");std::cout<<"PASS: paused seek, idle scheduling and prelude landing\n";
 }
 static void backgrounds(const std::filesystem::path&directory){
  BackgroundClock clock;clock.update(true,1,0);expectNear(clock.time(100),100,"background clock advances");clock.update(true,2,100);expectNear(clock.position(150),200,"speed changes preserve phase continuity");clock.update(false,2,150);expectNear(clock.position(1000),200,"pause freezes background phase");expectNear(clock.time(1000),150,"pause freezes texture sampling clock");clock.update(true,.5,1000);expectNear(clock.position(1200),300,"background resumes at selected speed");
  BackgroundSettings s;s.mode=1;s.fps=15;FluidBackground cached;
  for(int i=0;i<60;i++)cached.prepare(s,i*1000./60,i*1000./60);
  require(cached.gpuReady(),cached.gpuError.c_str());require(cached.revision==15,"60 FPS lyric drawing must build only 15 background textures per second");auto last=cached.pixels;auto revision=cached.revision;
  for(int i=0;i<10;i++)cached.prepare(s,983.333333,983.333333);require(cached.revision==revision&&cached.pixels==last,"static time reuses background texture");
  cached.prepare(s,2000,20000);require(cached.pixels!=last,"fluid background changes over time");auto soft=cached.pixels;s.mode=3;cached.prepare(s,2000,20000);require(cached.pixels!=soft,"dark and soft styles differ");
  uint64_t softSum=0,darkSum=0;for(size_t p=0;p<soft.size();p+=4){softSum+=soft[p]+soft[p+1]+soft[p+2];darkSum+=cached.pixels[p]+cached.pixels[p+1]+cached.pixels[p+2];}require(darkSum<softSum,"dark fluid preserves stronger text contrast");
  std::vector<uint8_t>red(16,0);for(int p=0;p<16;p+=4){red[p+2]=255;red[p+3]=255;}auto palette=coverBackgroundPalette(red,2,2);require(palette[0].r>palette[0].b,"cover quadrants determine background palette");cached.setPalette(palette);cached.prepare(s,2000,20000);require(cached.pixels!=soft,"cover changes invalidate cached texture");
  std::vector<uint8_t>blue(16,0),green(16,0);for(int p=0;p<16;p+=4){blue[p]=255;blue[p+3]=255;green[p+1]=255;green[p+3]=255;}
  s.mode=1;cached.setCover(red,2,2,3000,true);cached.prepare(s,3000,20000);auto before=cached.pixels;
  cached.setCover(blue,2,2,3000);cached.prepare(s,3000,20000);require(cached.pixels==before,"new artwork starts from previous background, without default flash");
  cached.prepare(s,3250,20000);require(cached.pixels!=before,"artwork transition has intermediate pixels");auto middle=cached.pixels;
  cached.setCover(green,2,2,3250);cached.prepare(s,3250,20000);int largest=0;for(size_t i=0;i<middle.size();i++)largest=std::max(largest,std::abs(int(middle[i])-int(cached.pixels[i])));require(largest<=2,"rapid switch continues from current blend");
  cached.prepare(s,4000,20000);require(cached.pixels[1]>cached.pixels[0]&&cached.pixels[1]>cached.pixels[2],"transition reaches current artwork");
  cached.setCover({},0,0,4000);cached.prepare(s,4000,20000);require(cached.pixels[1]>cached.pixels[0],"missing artwork also starts from retained visual");
  Fixture f;auto&a=f.a;f.lines();f.layout(4,4);
  a.cover.width=a.cover.height=2;a.cover.pixels=red;a.fluid.setCover(red,2,2,0,true);a.fluid.prepare(s,0,0);auto retained=a.fluid.pixels;a.loadCover(L"https://example.invalid/pending");a.fluid.prepare(s,0,0);require(a.fluid.pixels==retained&&a.cover.pixels==red,"pending download retains artwork and background");
  auto stale=std::make_unique<CoverData>();stale->epoch=a.coverEpoch-1;stale->width=stale->height=2;stale->pixels=blue;a.onCover(std::move(stale));require(a.cover.pixels==red,"late cover result cannot replace current artwork");
  for(int mode:{1,2,3})for(int fps:{10,15,30,45,60}){Json options=Json::obj();options["mode"]=mode;options["fps"]=fps;options["animated"]=true;a.readBackground(options);f.interval(21000);expectNear(a.framePeriod,1000./fps,"background uses selected independent frame rate");}
  a.background.animated=false;expectNear(f.interval(21000),200,"static fluid does not force background frames");a.background.mode=0;a.background.animated=true;expectNear(f.interval(21000),200,"gradient preserves idle scheduling");
  a.background.mode=1;a.playing=false;a.update();require(!a.frameRunning,"paused background stops scheduling");a.playing=true;a.valid=false;a.update();require(!a.frameRunning,"stalled playback freezes background");
  a.valid=true;a.lines.clear();a.groups.clear();a.index=-1;a.focusIndex=-1;a.background.fps=15;f.interval(0);expectNear(a.framePeriod,1000./15,"fluid still animates when a playing track has no lyrics");
  Json invalid=Json::obj();invalid["mode"]=-100;invalid["fps"]=1;invalid["speed"]=1000;a.readBackground(invalid);require(a.background.mode==0&&a.background.fps==60&&a.background.speed==3,"native background options are bounded");
  if(!directory.empty()){
   a.background={3,false,2,30};a.settingsPath=(directory/"background-settings.json").wstring();a.save();require(std::filesystem::exists(a.settingsPath),"background settings saved");
   auto previous=env(L"MINILYRICS_SETTINGS");SetEnvironmentVariableW(L"MINILYRICS_SETTINGS",a.settingsPath.c_str());App restored;SetEnvironmentVariableW(L"MINILYRICS_SETTINGS",previous.empty()?nullptr:previous.c_str());require(restored.background==a.background,"background options restore from settings file");
  }
  a.fluid.setPalette(defaultBackgroundPalette());Surface surface;surface.g.init(surface.target.get(),a.bgA,a.bgB,CoverData(),600,160);a.width=600;a.height=160;a.testWallMs=1000;a.backgroundClock=BackgroundClock();a.backgroundClock.update(true,1,1000);
  auto frame=[&](){surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));a.drawBackground(surface.g,D2D1::RoundedRect(D2D1::RectF(.5f,.5f,599.5f,159.5f),17,17));check(surface.target->EndDraw());GdiFlush();return std::vector<BYTE>(surface.pixels,surface.pixels+Surface::width*Surface::height*4);};
  std::vector<BYTE>previous;
  for(int mode:{0,1,2,3}){Json options=Json::obj();options["mode"]=mode;a.readBackground(options);auto image=frame();if(!previous.empty())require(image!=previous,"each mode changes actual background pixels");previous=image;if(!directory.empty())surface.save(directory/("background-mode-"+std::to_string(mode)+".bmp"));}
  a.background.mode=1;a.fluid.invalidate();auto initial=frame();a.testWallMs=3000;auto moving=frame();require(initial!=moving,"native dynamic rendering changes pixels");a.backgroundClock.update(false,1,3000);auto frozen=frame();revision=a.fluid.revision;a.testWallMs=8000;require(frame()==frozen&&a.fluid.revision==revision,"paused render is pixel-identical without texture rebuild");
 }
 static void optimizations(const std::filesystem::path&directory){
  Json metrics=Json::obj();
  Fixture f;auto&a=f.a;f.lines(1);f.layout(10);a.songId="optimization";
  auto small=a.state("host-state"),full=a.diagnostics("inspection");
  require(small.object.count("groups")==0&&small.object.count("coverLeftPx")==0,"normal replies omit layout diagnostics");
  require(small["songId"].s()==a.songId&&small["lyricCount"].n()==40&&small["positionMs"].n()==a.position,"compact replies retain playback and acknowledgement data");
  require(full["groups"].array.size()==a.groups.size()&&full["groups"].array.size()>0,"explicit inspection retains full layout diagnostics");
  require(a.state("background-state")["background"]["mode"].n()==0,"background acknowledgement retains configuration");
  metrics["normalReplyBytes"]=int(small.dump().size());metrics["diagnosticReplyBytes"]=int(full.dump().size());
  require(small.dump().size()*5<full.dump().size(),"normal reply is substantially smaller than inspection");
  size_t bytes=0;double start=nowMs();for(int i=0;i<100;i++)bytes+=a.state("host-state").dump().size();metrics["normalReplyMeanMs"]=(nowMs()-start)/100;
  start=nowMs();for(int i=0;i<100;i++)bytes+=a.diagnostics("inspection").dump().size();metrics["diagnosticReplyMeanMs"]=(nowMs()-start)/100;require(bytes>0,"serialization benchmark consumes its results");
  // Run the production window procedure off screen, without a player or socket.
  DestroyWindow(a.hwnd);a.hwnd=nullptr;a.renderVisible=false;
  check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,a.factory.put()));
  a.makeFormat(a.metaFormat,11.5f,DWRITE_FONT_WEIGHT_NORMAL);
  WNDCLASSW cls{};cls.lpfnWndProc=App::procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"MiniLyrics.OptimizationTest";RegisterClassW(&cls);
  a.hwnd=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"Isolated optimization checks",WS_POPUP,-30000,-30000,600,240,nullptr,nullptr,cls.hInstance,&a);
  require(a.hwnd!=nullptr,"create offscreen production-procedure window");a.geometry();a.renderVisible=true;
  a.background.mode=1;a.playing=true;a.valid=true;a.cover.width=a.cover.height=2;
  a.cover.pixels={30,60,220,255,150,20,80,255,80,180,30,255,180,100,20,255};
  ShowWindow(a.hwnd,SW_SHOWNOACTIVATE);
  auto calls=a.renderCalls;ValidateRect(a.hwnd,nullptr);
  a.update();a.update();a.update();
  require(a.renderCalls==calls&&GetUpdateRect(a.hwnd,nullptr,FALSE),"a burst invalidates once without synchronous paints");
  UpdateWindow(a.hwnd);require(a.renderCalls==calls+1&&a.error.empty(),"one WM_PAINT draws the final batched state");
  require(a.fluid.gpuReady(),a.fluid.gpuError.c_str());
  metrics["batchedUpdates"]=3;metrics["batchPaints"]=int(a.renderCalls-calls);
  auto brush=a.graphics.brush.get();auto cover=a.graphics.cover.get();auto bitmap=a.graphics.background.get();auto bitmapBrush=a.graphics.backgroundBrush.get();
  auto image=std::make_unique<CoverData>();image->epoch=a.coverEpoch;image->width=image->height=2;image->pixels=std::vector<BYTE>(16,255);a.onCover(std::move(image));
  require(a.graphics.brush.get()==brush&&a.graphics.cover.get()==cover&&a.graphics.background.get()==bitmap&&a.graphics.backgroundBrush.get()==bitmapBrush,"cover update reuses brush, same-size cover and background resources");
  SetWindowPos(a.hwnd,nullptr,-30000,-30000,620,240,SWP_NOZORDER|SWP_NOACTIVATE);a.renderVisible=true;
  require(a.graphics.brush.get()==brush&&a.graphics.cover.get()==cover&&a.graphics.background.get()==bitmap&&a.graphics.backgroundBrush.get()==bitmapBrush,"successful HWND resize retains device resources");
  expectNear(a.graphics.gradient->GetEndPoint().x,620,"resize updates gradient geometry");
  UpdateWindow(a.hwnd);require(a.error.empty(),"retained resources still draw after cover update and resize");
  ShowWindow(a.hwnd,SW_SHOWNOACTIVATE);require(a.renderVisible,"show message enables rendering");UpdateWindow(a.hwnd);
  ShowWindow(a.hwnd,SW_HIDE);require(!a.renderVisible&&!a.frameRunning&&!a.frameArmed,"hide message stops rendering and timer");
  auto hiddenCalls=a.renderCalls,hiddenBuilds=a.lineBuildCalls,hiddenTextures=a.fluid.revision;double phase=a.backgroundClock.position(a.animationWall());
  Json clock=Json::obj();clock["type"]="clock";clock["songId"]=a.songId;clock["playing"]=true;clock["valid"]=true;clock["position"]=43000;
  a.testWallMs+=1000;a.receive(clock);a.paint();
  require(a.position==43000&&a.valid&&a.playing,"hidden window still accepts playback updates");
  require(a.renderCalls==hiddenCalls&&a.lineBuildCalls==hiddenBuilds&&a.fluid.revision==hiddenTextures,"hidden updates do not render, build lyrics or generate textures");
  metrics["hiddenPaints"]=int(a.renderCalls-hiddenCalls);
  expectNear(a.backgroundClock.position(a.animationWall()),phase,"hidden background animation stays frozen");
  ShowWindow(a.hwnd,SW_SHOWNOACTIVATE);require(a.renderVisible&&a.index==21&&a.focusIndex==21,"show restores the current playback row without an old flight");UpdateWindow(a.hwnd);
  ShowWindow(a.hwnd,SW_SHOWMINNOACTIVE);require(!a.renderVisible&&!a.frameRunning,"minimize also stops render scheduling");expectNear(a.width,620,"minimize does not rebuild geometry to an icon-sized window");
  ShowWindow(a.hwnd,SW_SHOWNOACTIVATE);require(a.renderVisible&&!IsIconic(a.hwnd)&&a.index==21,"restore from minimized resumes at the current row");
  FluidBackground fallback;fallback.failed=true;fallback.gpuError="isolated device failure";BackgroundSettings settings;
  fallback.prepare(settings,0,0);auto first=fallback.pixels;fallback.prepare(settings,1000,1000);
  require(!fallback.gpuReady()&&fallback.gpuError=="isolated device failure"&&fallback.pixels!=first,"known GPU failure uses the persistent CPU fallback");
  auto revision=fallback.revision;require(!fallback.prepare(settings,1000,1000)&&fallback.revision==revision,"fallback also reuses a cached frame");
  std::cout<<"PASS: compact status, coalesced paint, resource reuse, hide/show/minimize and persistent fallback\n";
  if(!directory.empty()){std::ofstream out(directory/"optimization-results.json");out<<metrics.dump();}
 }
 static void resizeRendering(const std::filesystem::path&directory){
  // Three critical scenarios: independent rates, batched programmatic resize,
  // and actual capture-based live resize with a frozen/resumed background.
  Fixture f;auto&a=f.a;f.lines();f.layout(10);a.background={};
  require(a.background.fps==60,"new background default is 60 FPS");
  FluidBackground cached;
  Json metrics=Json::obj();metrics["rates"].kind=Json::Array;
  for(int rate:{10,15,30,45,60}){
   Json options=Json::obj();options["fps"]=rate;a.readBackground(options);f.interval(21000);expectNear(a.framePeriod,1000./rate,"requested background rate reaches the scheduler");
   auto before=cached.revision;
   for(int i=0;i<60;i++)cached.prepare(a.background,i*1000./60,i*1000./60);
   require(cached.gpuReady(),cached.gpuError.c_str());require(cached.revision-before==unsigned(rate),"independent texture rate matches each configured limit");
   metrics["rates"].array.push_back(rate);
  }
  DestroyWindow(a.hwnd);a.hwnd=nullptr;a.renderVisible=false;
  check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,a.factory.put()));a.makeFormat(a.metaFormat,11.5f,DWRITE_FONT_WEIGHT_NORMAL);
  WNDCLASSW cls{};cls.lpfnWndProc=App::procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"MiniLyrics.ResizeTest";RegisterClassW(&cls);
  a.hwnd=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"Isolated resize checks",WS_POPUP,-30000,-30000,460,130,nullptr,nullptr,cls.hInstance,&a);
  require(a.hwnd!=nullptr,"create production-procedure resize window");ShowWindow(a.hwnd,SW_SHOWNOACTIVATE);a.renderVisible=true;a.coverSince=-1000;a.background.fps=30;
  a.update();UpdateWindow(a.hwnd);require(a.error.empty()&&a.fluid.gpuReady(),"initial production frame is ready");
  auto brush=a.graphics.brush.get();auto bitmap=a.graphics.background.get();auto layouts=a.lineBuildCalls;
  auto old=a.target->GetPixelSize();auto calls=a.renderCalls;
  SetWindowPos(a.hwnd,nullptr,0,0,540,192,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);
  auto pending=a.target->GetPixelSize();require(pending.width==old.width&&pending.height==old.height&&a.renderCalls==calls,"programmatic resize retains the completed buffer until paint");
  UpdateWindow(a.hwnd);auto rendered=a.target->GetPixelSize();require(rendered.width==540&&rendered.height==192&&a.renderCalls==calls+1,"target resize and full draw complete in one paint");
  ValidateRect(a.hwnd,nullptr);auto changes=a.layoutChanges;SendMessageW(a.hwnd,WM_SIZE,SIZE_RESTORED,MAKELPARAM(540,192));
  require(!GetUpdateRect(a.hwnd,nullptr,FALSE)&&a.layoutChanges==changes&&!a.geometry(),"duplicate size messages do not rebuild or repaint");
  a.message(WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(535,187));require(a.resizing&&GetCapture()==a.hwnd&&!a.backgroundClock.running,"resize capture freezes the background clock");
  UpdateWindow(a.hwnd);auto revision=a.fluid.revision;auto frozen=a.fluid.pixels;double phase=a.backgroundClock.position(a.animationWall());calls=a.renderCalls;
  for(SIZE size:std::vector<SIZE>{{600,210},{900,240},{360,130},{540,192},{700,210}}){
   a.testWallMs+=100;SetWindowPos(a.hwnd,nullptr,0,0,size.cx,size.cy,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOCOPYBITS);
   auto actual=a.target->GetPixelSize();require(actual.width==unsigned(size.cx)&&actual.height==unsigned(size.cy)&&a.error.empty(),"live resize paints a full frame before returning");
  }
  require(a.renderCalls-calls==5&&a.fluid.revision==revision&&a.fluid.pixels==frozen,"live size changes reuse the background without repeated GPU generation");
  expectNear(a.backgroundClock.position(a.animationWall()),phase,"resizing never advances the frozen background");
  require(a.graphics.brush.get()==brush&&a.graphics.background.get()==bitmap&&a.lineBuildCalls==layouts,"resizing retains drawing resources and cached lyric layouts");
  FluidBackground retained;retained.setSize(a.width,a.height);retained.prepare(a.background,a.backgroundClock.time(a.animationWall()),phase);
  auto cover=std::make_unique<CoverData>();cover->epoch=a.coverEpoch;cover->width=cover->height=2;cover->pixels={20,50,220,255,20,50,220,255,20,50,220,255,20,50,220,255};a.onCover(std::move(cover));UpdateWindow(a.hwnd);
  require(a.fluid.revision==revision&&a.fluid.pixels==frozen,"artwork arriving during resize waits without replacing the displayed background");
  a.message(WM_LBUTTONUP,0,0);require(!a.resizing&&GetCapture()!=a.hwnd&&a.backgroundClock.running,"release capture resumes normal background scheduling");
  UpdateWindow(a.hwnd);require(a.fluid.revision==revision+1&&a.fluid.pixels!=frozen,"final dimensions generate one updated background after release");
  require(a.fluid.pixels==retained.pixels,"queued artwork starts its crossfade from retained artwork after resize");
  double resumed=a.backgroundClock.position(a.animationWall());a.testWallMs+=100;a.update();UpdateWindow(a.hwnd);require(a.backgroundClock.position(a.animationWall())>resumed&&a.fluid.revision>revision+1,"background motion continues after resizing");
  // Capture loss uses the same completion path, including a paused player.
  a.playing=false;a.update();UpdateWindow(a.hwnd);a.message(WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(695,205));
  SetWindowPos(a.hwnd,nullptr,0,0,540,192,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOCOPYBITS);revision=a.fluid.revision;ReleaseCapture();UpdateWindow(a.hwnd);
  require(!a.resizing&&!a.backgroundClock.running&&a.fluid.revision==revision+1,"capture loss refreshes final dimensions while keeping a paused background frozen");
  metrics["liveSizeChanges"]=5;metrics["backgroundBuildsDuringResize"]=0;metrics["finalBackgroundBuilds"]=1;metrics["resourceReuse"]=true;metrics["captureLoss"]=true;
  if(!directory.empty()){std::ofstream out(directory/"resize-refresh-results.json");out<<metrics.dump();}
  std::cout<<"PASS: background rates, complete resize frames, frozen texture reuse and capture completion\n";
 }
 static void wordScrolling(const std::filesystem::path&directory){
  Fixture f;auto&a=f.a;a.width=360;a.height=130;a.textX=20;a.viewportBottom=102;a.coverSince=-1000;
  Line line;line.time=0;line.duration=30000;line.translation=std::wstring(100,L'T');
  for(int i=0;i<48;i++){line.text+=L"词";line.words.push_back({L"词",i<24?i*60.:8000+(i-24)*200.,i<24?60.:200.});}
  a.lines={line};a.index=0;a.position=1000;a.playing=true;a.valid=true;f.layout(0);a.group(0).y.snap(40);
  auto&g=a.group(0);float available=a.width-a.textX-12,total=a.rowWidth(g),overflow=total-available;
  require(overflow>available,"fixture must exceed the viewport by more than its full width");
  double maximumDeviation=0;
  for(float zoom:{1.f,16.f/21.f})for(double time:{1350.,10500.}){
   g.zoom.snap(zoom);
   auto found=std::find_if(g.glyphs.begin(),g.glyphs.end(),[&](const Glyph&w){return time>=w.time&&time<w.time+w.duration;});require(found!=g.glyphs.end(),"fixture has a currently sung word");
   float cursor=found->x+found->layout.width*float((time-found->time)/found->duration);
   float x=cursor*zoom-(total*zoom-available)*a.horizontalPhase(g,time,0);
   require(x>=0&&x<=available,"currently sung position must remain inside the viewport despite uneven pacing");
   require(std::abs(x-available*.5f)<.05f,"middle-of-line word position follows the center of the viewport");maximumDeviation=std::max(maximumDeviation,double(std::abs(x-available*.5f)));
  }
  g.zoom.snap(1);
  float gap=a.horizontalPhase(g,2000,0);require(std::abs(gap-a.horizontalPhase(g,7000,0))<.0001f,"word gaps hold horizontal position");
  float before=overflow*a.horizontalPhase(g,1379.9,0),after=overflow*a.horizontalPhase(g,1380.1,0);require(std::abs(after-before)<1,"contiguous word boundaries keep horizontal motion continuous");
  require(a.horizontalPhase(g,-1,0)==0&&a.horizontalPhase(g,20000,0)==1,"word-following clamps to the start and end of the line");
  a.translation=false;g.translated=false;expectNear(f.interval(4000),200,"word gap without translated marquee returns to idle scheduling");expectNear(f.interval(1350),34,"active words retain lyric animation frames");
  a.translation=true;g.translated=true;
  Surface surface;auto frame=[&](double time){surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));surface.target->PushAxisAlignedClip(D2D1::RectF(a.textX,0,a.width-12,130),D2D1_ANTIALIAS_MODE_ALIASED);a.drawGroup(surface.g,g,0,time);surface.target->PopAxisAlignedClip();check(surface.target->EndDraw());GdiFlush();return std::vector<BYTE>(surface.pixels,surface.pixels+Surface::width*Surface::height*4);};
  auto following=frame(1350);if(!directory.empty())surface.save(directory/"word-following.bmp");
  int subtitleTop=int(std::ceil(40+g.subtitleOffset(1.f))),subtitleBottom=std::min(130,int(std::ceil(40+g.renderedHeight(1.f))));
  for(auto&w:g.glyphs){w.time+=20000;}
  auto delayed=frame(1350);if(!directory.empty())surface.save(directory/"word-timing-delayed.bmp");
  require(following!=delayed,"changing word timing changes actual native original pixels");
  size_t begin=subtitleTop*Surface::width*4,end=subtitleBottom*Surface::width*4;
  require(std::equal(following.begin()+begin,following.begin()+end,delayed.begin()+begin),"translation pixels retain the existing line-time marquee despite word timing changes");
  for(auto&w:g.glyphs){w.time-=20000;}
  a.browseIndex=0;require(a.horizontalPhase(g,1350,0)==0,"manual browsing keeps the original aligned to its start");a.browseIndex=-1;
  a.playing=false;a.position=1350;auto paused=frame(a.current());a.testWallMs+=1000;require(frame(a.current())==paused,"paused playback holds word following without a separate scrolling clock");
  float atEnd=a.horizontalPhase(g,20000,0),atStart=a.horizontalPhase(g,0,0);require(atEnd==1&&atStart==0,"backward seeking immediately reanchors word following");
  Line plain;plain.time=0;plain.duration=4000;plain.text=std::wstring(100,L'W');a.lines={plain};a.groups.clear();a.focusIndex=-999;a.playing=true;f.layout(0);auto&untimed=a.group(0);
  require(a.horizontalPhase(untimed,899,0)==0&&std::abs(a.horizontalPhase(untimed,2100,0)-.5f)<.0001f&&a.horizontalPhase(untimed,3300,0)==1,"untimed long lyrics retain the existing delay and line-duration marquee");
  if(!directory.empty()){std::ofstream report(directory/"word-scroll-results.json");report<<"{\"pass\":true,\"scenarios\":3,\"maximumCursorCenterDeviationDip\":"<<maximumDeviation<<",\"nativePixels\":true,\"translationUnchanged\":true,\"wordGapsHold\":true,\"browsePauseSeek\":true,\"untimedFallback\":true}";}
  std::cout<<"PASS: timed word visibility, native pixels, translation independence, browse/pause/seek and untimed fallback\n";
 }
 static void cleanup(const std::filesystem::path&directory){
  Fixture f;auto&a=f.a;f.lines(1);f.layout(10);a.playing=false;
  auto id=a.group(10).identity,builds=a.lineBuildCalls;
  Json clock=Json::obj();clock["type"]="clock";clock["position"]=25000;clock["playing"]=false;clock["valid"]=true;a.receive(clock);
  Json metrics=Json::obj();metrics["correctionAddedLayouts"]=double(a.lineBuildCalls-builds);bool cached=a.group(10).identity==id&&a.lineBuildCalls-builds==2;
  builds=a.lineBuildCalls;a.setRenderVisible(false);a.setRenderVisible(true);metrics["showAddedLayouts"]=double(a.lineBuildCalls-builds);bool shown=a.lineBuildCalls==builds;
  Json lyrics=Json::obj();lyrics["type"]="lyrics";lyrics["songId"]="";lyrics["lines"].kind=Json::Array;
  for(size_t i=0;i<a.lines.size();i++){Json line=Json::obj();line["time"]=a.lines[i].time;line["duration"]=a.lines[i].duration;line["text"]=narrow(a.lines[i].text);line["translation"]=i==12?"Updated translation":narrow(a.lines[i].translation);lyrics["lines"].array.push_back(line);}
  builds=a.lineBuildCalls;id=a.group(10).identity;a.receive(lyrics);bool changed=a.group(12).model.translation==L"Updated translation"&&a.group(10).identity==id&&a.lineBuildCalls==builds+1;
  metrics["changedLineAddedLayouts"]=double(a.lineBuildCalls-builds);
  SetWindowPos(a.hwnd,nullptr,-30000,-30000,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);ShowWindow(a.hwnd,SW_SHOWNOACTIVATE);
  ValidateRect(a.hwnd,nullptr);Json style=Json::obj();style["type"]="style";style["curve"]=a.lineCurve;style["stagger"]=a.stagger;a.receive(style);bool unchanged=!GetUpdateRect(a.hwnd,nullptr,FALSE);
  if(!directory.empty()){
   a.settingsPath=(directory/"no-op-settings.json").wstring();{std::ofstream settings(directory/"no-op-settings.json");settings<<"unchanged";}
   Json background=a.backgroundState();background["type"]="background";a.receive(background);std::ifstream saved(directory/"no-op-settings.json");std::string content((std::istreambuf_iterator<char>(saved)),{});unchanged=unchanged&&content=="unchanged"&&!GetUpdateRect(a.hwnd,nullptr,FALSE);
   background["mode"]=3;a.receive(background);require(a.background.mode==3&&GetUpdateRect(a.hwnd,nullptr,FALSE),"changed background still applies and invalidates");a.background.mode=0;
  }
  metrics["unchangedSettingsSkipped"]=unchanged;
  bool visiblePixels=true;
  for(bool words:{false,true}){
   Fixture p;auto&v=p.a;p.lines();v.height=160;v.viewportBottom=132;v.textX=20;
   for(auto&line:v.lines){line.time+=5000;if(words)line.words={{line.text,line.time,line.duration}};}
   v.position=26000;v.index=v.rowAt(v.position);v.playing=false;v.updatePlaceholders();p.layout(v.index);v.seekTo(0);v.update();
   v.testWallMs+=900;v.applyFlight(v.animationWall());v.updateIndicators();Surface actual,expected;
   actual.target->BeginDraw();actual.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));v.drawLyrics(actual.g,v.current());check(actual.target->EndDraw());
   expected.target->BeginDraw();expected.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));v.drawInterlude(expected.g,v.preludeDots,v.textX,v.lyricAnchor(),v.current(),0,v.lines.front().time);v.drawGroup(expected.g,v.group(0),0,v.current());check(expected.target->EndDraw());GdiFlush();
   bool same=std::equal(actual.pixels,actual.pixels+Surface::width*Surface::height*4,expected.pixels);visiblePixels=visiblePixels&&same;metrics[words?"wordPreludeOnlyPreview":"plainPreludeOnlyPreview"]=same;
   if(!directory.empty())actual.save(directory/(words?"word-prelude-landing.bmp":"plain-prelude-landing.bmp"));
  }
  metrics["pass"]=cached&&shown&&changed&&visiblePixels&&unchanged;metrics["scenarios"]=3;
  if(!directory.empty()){std::ofstream report(directory/"cleanup-results.json");report<<metrics.dump();}
  require(cached,"clock correction retains cached glyphs and builds only newly visible rows");require(shown,"showing unchanged lyrics reuses cached layouts");require(changed,"one changed translation rebuilds only its own group");require(unchanged,"unchanged settings do not repaint or overwrite settings");require(visiblePixels,"plain and word lyrics both hide non-preview rows when landing in prelude");
  std::cout<<"PASS: cached correction/show, changed-line refresh and shared plain/word visibility\n";
 }
 static void preludeRows(const std::filesystem::path&directory){
  struct Sockets {Sockets(){WSADATA data{};require(WSAStartup(MAKEWORD(2,2),&data)==0,"initialize isolated seek bridge");}~Sockets(){WSACleanup();}} sockets;
  Fixture f;auto&a=f.a;a.makeFormat(a.metaFormat,11.5f,DWRITE_FONT_WEIGHT_NORMAL);a.height=160;a.viewportBottom=132;a.textX=20;
  auto track=[&](const char*id){Json m=Json::obj();m["type"]="track";m["id"]=id;m["title"]=id;m["duration"]=30000;a.receive(m);};
  auto lyrics=[&](const char*id,double first,bool gap=false,bool words=false){Json m=Json::obj();m["type"]="lyrics";m["songId"]=id;m["lines"].kind=Json::Array;if(gap){Json l=Json::obj();l["time"]=0;l["duration"]=first;l["text"]="";l["isInterlude"]=true;m["lines"].array.push_back(l);}for(int i=0;i<3;i++){Json l=Json::obj();l["time"]=first+i*5000;l["duration"]=5000;l["text"]=std::string(id)+" lyric "+std::to_string(i);l["translation"]="Translation";if(words){Json word=Json::obj();word["text"]=l["text"];word["time"]=l["time"];word["duration"]=5000;l["words"].kind=Json::Array;l["words"].array.push_back(word);}m["lines"].array.push_back(l);}a.receive(m);};
  auto clock=[&](double time,bool playing=true){Json m=Json::obj();m["type"]="clock";m["position"]=time;m["valid"]=true;m["playing"]=playing;a.receive(m);};
  Surface surface,reference;
  auto render=[&](){surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));a.drawLyrics(surface.g,a.current()+a.offset);check(surface.target->EndDraw());GdiFlush();return std::vector<BYTE>(surface.pixels,surface.pixels+Surface::width*Surface::height*4);};
  auto click=[&](int id,double target){auto&g=a.group(id);int x=int(std::round((a.textX+8)*a.scale())),y=int(std::round((g.y.value(a.motionNow())+g.mainHeight*g.zoom.value(a.motionNow())*.5f)*a.scale()));a.hover=false;a.message(WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));require(a.seekPending&&a.position==target,"visible lyric click enters the normal seek request path");require(!a.flight.rows.empty(),"prelude click retains the normal seek flight");};
  track("early-lyrics");lyrics("early-lyrics",6000);clock(1000);a.testWallMs+=1000;a.update();
  auto before=render();if(!directory.empty())surface.save(directory/"prelude-preview.bmp");
  auto&g=a.group(0);bool visible=a.groupVisible(g,a.motionNow());Json metrics=Json::obj();metrics["visibleNormalPreview"]=visible;metrics["pass"]=false;if(!directory.empty()){std::ofstream report(directory/"prelude-results.json");report<<metrics.dump();}
  require(a.lyricView()==App::LyricView::Prelude&&visible,"prelude preview must be a visible normal lyric and click target");
  expectNear(g.zoom.value(a.motionNow()),ContextFontSize/CurrentFontSize,"prelude preview retains the context size");expectNear(g.y.value(a.motionNow()),a.lyricAnchor()+a.mainRowHeight+GroupGap,"preview stays below the prelude dots");
  DWRITE_FONT_WEIGHT weight{};check(g.main.value->GetFontWeight(0,&weight,nullptr));require(weight==DWRITE_FONT_WEIGHT_SEMI_BOLD,"preview uses the original lyric font weight");
  reference.target->BeginDraw();reference.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));a.drawGroup(reference.g,g,0,a.current());check(reference.target->EndDraw());GdiFlush();
  int top=int(std::floor(g.y.value(a.motionNow()))),bottom=std::min(Surface::height,int(std::ceil(g.y.value(a.motionNow())+g.renderedHeight(g.zoom.value(a.motionNow())))));
  require(std::equal(before.begin()+top*Surface::width*4,before.begin()+bottom*Surface::width*4,reference.pixels+top*Surface::width*4),"prelude preview pixels match the normal semibold lyric and translation");
  a.bridge=std::make_unique<Bridge>(0,std::string(32,'p'),[](Json){});click(0,6000);auto firstFrame=render();require(std::equal(before.begin()+top*Surface::width*4,before.begin()+bottom*Surface::width*4,firstFrame.begin()+top*Surface::width*4),"prelude seek starts from the displayed original and translation pixels");a.testWallMs+=900;a.update();require(a.flight.rows.empty()&&a.index==0,"prelude click lands on the first lyric");
  a.cancelSeek();a.seekTo(0);a.update();a.testWallMs+=900;a.update();a.position=5800;a.update();a.testWallMs+=200;a.position=6000;a.update();
  require(a.index==0&&a.transitionActive,"natural first-line onset animates from the preview");a.testWallMs+=500;a.update();expectNear(a.group(0).zoom.value(a.motionNow()),1,"first line grows to the normal focus size");expectNear(a.group(0).y.value(a.motionNow()),a.lyricAnchor(),"first line returns to the normal focus anchor");
  // Clock-first arrival after a different song must replace the prior preview.
  track("late-lyrics");clock(1000);require(a.lyricView()==App::LyricView::Waiting,"clock-first track waits for its lyrics");lyrics("late-lyrics",10000,false,true);a.testWallMs+=1000;a.update();
  require(a.groupVisible(a.group(0),a.motionNow())&&a.group(0).model.text==L"late-lyrics lyric 0","late lyrics replace the old preview and become clickable");click(0,10000);a.testWallMs+=900;a.update();
  a.cancelSeek();a.seekTo(0);a.playing=false;a.update();a.testWallMs+=900;a.update();require(a.lyricView()==App::LyricView::Prelude&&a.flight.rows.empty(),"paused seek back lands in prelude");
  require(a.groupVisible(a.group(0),a.motionNow()),"returned prelude retains the same visible lyric target");click(0,10000);a.testWallMs+=900;a.update();require(a.flight.rows.empty()&&!a.frameRunning,"paused prelude click finishes its animation and sleeps");
  // RNP's explicit opening gap keeps the same ordinary-row input behavior.
  track("explicit-gap");lyrics("explicit-gap",6000,true);clock(1000);a.testWallMs+=1000;a.update();
  require(a.lyricView()==App::LyricView::Rows&&a.groupVisible(a.group(1),a.motionNow()),"explicit opening gap keeps the normal preview");click(1,6000);
  metrics["pass"]=true;metrics["scenarios"]=3;metrics["normalFontPixels"]=true;metrics["clickRequests"]=4;metrics["lateArrival"]=true;metrics["pausedReturn"]=true;
  if(!directory.empty()){std::ofstream report(directory/"prelude-results.json");report<<metrics.dump();}
  std::cout<<"PASS: prelude font/pixels and clicks, late lyrics and paused return, explicit opening gap\n";
 }
 static void movingFrames(const std::filesystem::path&directory){
  Fixture f;auto&a=f.a;DestroyWindow(a.hwnd);a.hwnd=nullptr;a.renderVisible=false;
  CancelWaitableTimer(a.frameTimer);CloseHandle(a.frameTimer);
  a.frameTimer=CreateWaitableTimerExW(nullptr,nullptr,0x00000002,TIMER_MODIFY_STATE|SYNCHRONIZE);
  require(a.frameTimer!=nullptr,"create high-resolution move test timer");
  check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,a.factory.put()));a.makeFormat(a.metaFormat,11.5f,DWRITE_FONT_WEIGHT_NORMAL);
  WNDCLASSW cls{};cls.lpfnWndProc=App::procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"MiniLyrics.MoveTest";RegisterClassW(&cls);
  a.hwnd=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"Isolated move checks",WS_POPUP,-30000,-30000,600,240,nullptr,nullptr,cls.hInstance,&a);
  require(a.hwnd!=nullptr,"create production-procedure move window");ShowWindow(a.hwnd,SW_SHOWNOACTIVATE);
  Line line;line.time=0;line.duration=60000;line.text=L"Moving lyric";line.words={{line.text,0,60000}};a.lines={line};a.layoutDirty=true;
  a.testWall=false;a.freezePlayback=false;a.position=1000;a.anchor=nowMs();a.playing=a.valid=true;a.fps=60;a.background.mode=2;a.background.fps=60;a.coverSince=-1000;
  a.cover.width=a.cover.height=2;a.cover.pixels={30,60,220,255,150,20,80,255,80,180,30,255,180,100,20,255};a.fluid.setCover(a.cover.pixels,2,2,0,true);
  a.update();UpdateWindow(a.hwnd);require(a.error.empty()&&a.frameArmed,"initial animation is ready");
  // Like the native modal move loop, dispatch window messages without servicing
  // the handle waited by App::run. No real mouse input or player is involved.
  auto pump=[&](double milliseconds,bool move=false){double end=nowMs()+milliseconds;MSG msg{};int step=0;while(nowMs()<end){if(move)SetWindowPos(a.hwnd,nullptr,-30000+(step++%20),-30000,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER);while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(msg.message!=WM_QUIT){TranslateMessage(&msg);DispatchMessageW(&msg);}}MsgWaitForMultipleObjectsEx(0,nullptr,5,QS_ALLINPUT,MWMO_INPUTAVAILABLE);}};
  SendMessageW(a.hwnd,WM_ENTERSIZEMOVE,0,0);UpdateWindow(a.hwnd);
  require(a.systemMoving&&a.modalFrames.active(),"system move forwards the existing timer");
  auto start=a.renderCalls,builds=a.lineBuildCalls,changes=a.layoutChanges,revision=a.fluid.revision;auto target=a.target.get();auto bitmap=a.graphics.background.get();
  double begin=nowMs();pump(500,true);double elapsed=nowMs()-begin;auto frames=a.renderCalls-start;auto backgroundFrames=a.fluid.revision-revision;
  // Keep one actual queued notification to verify it cannot survive this move.
  MSG stale{};double deadline=nowMs()+200;bool queued=false;while(nowMs()<deadline&&!(queued=PeekMessageW(&stale,a.hwnd,App::FrameReady,App::FrameReady,PM_REMOVE)!=FALSE))MsgWaitForMultipleObjectsEx(0,nullptr,5,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
  SendMessageW(a.hwnd,WM_EXITSIZEMOVE,0,0);
  Json metrics=Json::obj();metrics["messageOnlyPumpMs"]=elapsed;metrics["movingFrames"]=double(frames);metrics["requestedFps"]=60;metrics["pass"]=frames>=8;
  require(frames>=8,"native modal message pump must continue animation frames");
  require(backgroundFrames>=8&&a.fluid.gpuReady(),"GPU background continues alongside word animation");metrics["backgroundFrames"]=double(backgroundFrames);
  require(a.lineBuildCalls==builds&&a.layoutChanges==changes&&a.target.get()==target&&a.graphics.background.get()==bitmap,"moving keeps cached layout, target and background resources");
  require(!a.systemMoving&&!a.modalFrames.active()&&a.frameArmed,"release unregisters modal forwarding and restores the normal timer");
  require(queued&&!a.modalFrames.accepts(stale.wParam),"queued notification is invalid after release");
  auto armed=a.frameDeadline;SendMessageW(a.hwnd,App::FrameReady,stale.wParam,0);require(a.frameDeadline==armed&&a.frameArmed,"stale notification cannot advance the restored schedule");
  require(WaitForSingleObject(a.frameTimer,200)==WAIT_OBJECT_0,"normal frame handle resumes after moving");a.frameArmed=false;a.update();UpdateWindow(a.hwnd);
  a.playing=false;a.update();UpdateWindow(a.hwnd);SendMessageW(a.hwnd,WM_ENTERSIZEMOVE,0,0);UpdateWindow(a.hwnd);
  start=a.renderCalls;revision=a.fluid.revision;pump(150);
  require(!a.frameRunning&&!a.frameArmed&&a.renderCalls==start&&a.fluid.revision==revision,"paused moving does not poll or animate");
  SendMessageW(a.hwnd,WM_EXITSIZEMOVE,0,0);require(!a.modalFrames.active()&&!a.frameRunning,"paused release leaves no frame waiter or timer running");
  SendMessageW(a.hwnd,WM_ENTERSIZEMOVE,0,0);DestroyWindow(a.hwnd);a.hwnd=nullptr;
  require(!a.modalFrames.active()&&!a.frameRunning,"destroy during a move releases the waiter before closing the timer");
  metrics["scenarios"]=3;metrics["resumeAndStaleMessage"]=true;metrics["pausedFrames"]=0;metrics["resourceReuse"]=true;metrics["destroyCleanup"]=true;
  if(!directory.empty()){std::ofstream report(directory/"moving-results.json");report<<metrics.dump();}
  std::cout<<"PASS: moving frames="<<frames<<", background="<<backgroundFrames<<" in "<<elapsed<<" ms; resume, pause and cleanup passed\n";
 }
 static void run(const std::filesystem::path&directory){typography();motionGeometry();scheduling();pixels(directory);interludes(directory);browseInterludes(directory);seekFlights(directory);backgrounds(directory);std::cout<<"PASS: "<<checks<<" native checks (DirectWrite, lyric/background pixels, animation geometry, settings and scheduling)\n";}
};
int NativeTest::checks=0;
int main(int argc,char**argv){try{auto directory=argc>1?std::filesystem::path(argv[1]):std::filesystem::path();if(argc>2&&std::strcmp(argv[2],"font")==0)NativeTest::fontSizes(directory);else if(argc>2&&std::strcmp(argv[2],"flight")==0)NativeTest::seekFlights(directory);else if(argc>2&&std::strcmp(argv[2],"cleanup")==0)NativeTest::cleanup(directory);else if(argc>2&&std::strcmp(argv[2],"prelude")==0){NativeTest::preludeRows(directory);NativeTest::interludes(directory);}else if(argc>2&&std::strcmp(argv[2],"moving")==0)NativeTest::movingFrames(directory);else if(argc>2&&std::strcmp(argv[2],"words")==0)NativeTest::wordScrolling(directory);else if(argc>2&&std::strcmp(argv[2],"resize")==0)NativeTest::resizeRendering(directory);else if(argc>2&&std::strcmp(argv[2],"background")==0){NativeTest::backgrounds(directory);std::cout<<"PASS: "<<NativeTest::checks<<" focused background checks\n";}else if(argc>2&&std::strcmp(argv[2],"optimization")==0){NativeTest::optimizations(directory);NativeTest::seekFlights(directory);std::cout<<"PASS: "<<NativeTest::checks<<" focused optimization checks\n";}else NativeTest::run(directory);return 0;}catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
