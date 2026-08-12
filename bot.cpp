//+------------------------------------------------------------------+
//|                                           Beast_Top10_Simple.mq4 |
//|  Simple Top-10 Legacy v1 mean reversion basket EA                |
//|                                                                  |
//|  Logic:                                                          |
//|  - Fixed 10-pair basket                                          |
//|  - Legacy v1 NB-TMA slope                                        |
//|  - SELL if slope > 0                                             |
//|  - BUY  if slope < 0                                             |
//|  - Open at new signal bar + delay                               |
//|  - Close previous basket, then open new basket                   |
//|  - Fixed starting lots per pair = 0.10                           |
//+------------------------------------------------------------------+
#property strict

input string           Pairs              = "EURGBP,EURCAD,NZDCHF,CADCHF,GBPCAD,USDCAD,GBPCHF"; // Pairs
input ENUM_TIMEFRAMES  SignalTimeframe    = PERIOD_D1; // SignalTimeframe
input int              MinutesAfterBar    = 65;         // MinutesAfterBar
input int              TmaPeriod          = 20;        // TmaPeriod
input int              AtrPeriod          = 100;       // AtrPeriod
input double           Threshold          = 0.2;       // Threshold
input int              MagicNumber        = 770;  // MagicNumber
input double           BaseLots           = 0.30;      // BaseLots
input bool             UseCompounding     = True;     // UseCompounding
input double           EquityStep         = 100000.0;  // EquityStep
input bool             ShowDashboard      = true;      // ShowDashboard
input bool             PrintDebug         = false;     // PrintDebug

enum ECycleState
{
   STATE_IDLE,
   STATE_CLOSING,
   STATE_OPENING,
   STATE_DONE
};

ECycleState g_state = STATE_IDLE;
datetime g_lastSignalBar = 0;
datetime g_lastProcessedBar = 0;
datetime g_closeStart = 0;

string g_pairList[];
int    g_pairCount = 0;

string DASH_PREFIX = "BEAST_DASH_";

//-------------------- Dashboard helpers --------------------//
struct DashRow
{
   string sym;
   double slope;
};

void ClearDashObjects()
{
   int total = ObjectsTotal(0, 0, -1);
   for(int i = total - 1; i >= 0; i--)
   {
      string nm = ObjectName(0, i, 0, -1);
      if(StringFind(nm, DASH_PREFIX, 0) == 0)
         ObjectDelete(0, nm);
   }
}

void PutDashLabel(string name, int x, int y, string txt, color clr, int fontSize = 10, string fontName = "Consolas")
{
   string obj = DASH_PREFIX + name;
   if(ObjectFind(0, obj) >= 0)
      ObjectDelete(0, obj);

   ObjectCreate(0, obj, OBJ_LABEL, 0, 0, 0);
   ObjectSetInteger(0, obj, OBJPROP_CORNER, CORNER_LEFT_UPPER);
   ObjectSetInteger(0, obj, OBJPROP_XDISTANCE, x);
   ObjectSetInteger(0, obj, OBJPROP_YDISTANCE, y);
   ObjectSetInteger(0, obj, OBJPROP_COLOR, clr);
   ObjectSetInteger(0, obj, OBJPROP_FONTSIZE, fontSize);
   ObjectSetString(0, obj, OBJPROP_FONT, fontName);
   ObjectSetString(0, obj, OBJPROP_TEXT, txt);
}

void SortDashDesc(DashRow &a[])
{
   for(int i = 0; i < ArraySize(a) - 1; i++)
      for(int j = i + 1; j < ArraySize(a); j++)
         if(a[j].slope > a[i].slope) { DashRow t = a[i]; a[i] = a[j]; a[j] = t; }
}

void SortDashAsc(DashRow &a[])
{
   for(int i = 0; i < ArraySize(a) - 1; i++)
      for(int j = i + 1; j < ArraySize(a); j++)
         if(a[j].slope < a[i].slope) { DashRow t = a[i]; a[i] = a[j]; a[j] = t; }
}

void SortDashAbsDesc(DashRow &a[])
{
   for(int i = 0; i < ArraySize(a) - 1; i++)
      for(int j = i + 1; j < ArraySize(a); j++)
         if(MathAbs(a[j].slope) > MathAbs(a[i].slope)) { DashRow t = a[i]; a[i] = a[j]; a[j] = t; }
}

string DashLine(string sym, double slope)
{
   string val = DoubleToString(slope, 2);
   if(slope >= 0.0)
      val = " " + val;
   return StringFormat("%-6s %s", sym, val);
}

void DrawDashBucket(string key, int colX, int startY, string title, DashRow &rows[], color clr)
{
   PutDashLabel(key + "_title", colX, startY, title, clr, 10, "Consolas");

   int y = startY + 20;
   for(int i = 0; i < ArraySize(rows); i++)
   {
      PutDashLabel(key + "_" + IntegerToString(i), colX, y, DashLine(rows[i].sym, rows[i].slope), clr, 10, "Consolas");
      y += 17;
   }
}

void DrawDashSeparators(int x1, int x2, int y0, int rows)
{
   string sep = "";
   for(int i = 0; i < rows; i++)
   {
      if(i > 0) sep += "\n";
      sep += "|";
   }
   PutDashLabel("SEP1", x1, y0, sep, clrDimGray, 10, "Consolas");
   PutDashLabel("SEP2", x2, y0, sep, clrDimGray, 10, "Consolas");
}


//+------------------------------------------------------------------+
int OnInit()
{
   ParsePairs();
   g_lastSignalBar = iTime(Symbol(), SignalTimeframe, 1);
   EventSetTimer(5);
   UpdateDashboard();
   return(INIT_SUCCEEDED);
}
//+------------------------------------------------------------------+
void OnDeinit(const int reason)
{
   EventKillTimer();
   ClearDashObjects();
   Comment("");
}
//+------------------------------------------------------------------+
void OnTimer(){ OnTick(); }
//+------------------------------------------------------------------+
void OnTick()
{
   datetime now          = TimeCurrent();
   datetime currentTfBar = iTime(Symbol(), SignalTimeframe, 0);
   datetime closedTfBar  = iTime(Symbol(), SignalTimeframe, 1);
   if(currentTfBar <= 0 || closedTfBar <= 0)
   {
      UpdateDashboard();
      return;
   }

   if(closedTfBar != g_lastSignalBar)
   {
      g_lastSignalBar = closedTfBar;
      g_state = STATE_IDLE;
      if(PrintDebug) Print("New closed signal bar: ", TimeToString(closedTfBar, TIME_DATE|TIME_MINUTES));
   }

   switch(g_state)
   {
      case STATE_IDLE:
         if(g_lastProcessedBar != g_lastSignalBar && now >= currentTfBar + MinutesAfterBar * 60)
         {
            g_state = STATE_CLOSING;
            g_closeStart = now;
            CloseAllTrades();
         }
         break;

      case STATE_CLOSING:
         if(CountOurTrades() == 0)
            g_state = STATE_OPENING;
         else if((int)(now - g_closeStart) > 60)
         {
            CloseAllTrades();
            g_closeStart = now;
         }
         break;

      case STATE_OPENING:
         OpenTodaysTrades();
         g_lastProcessedBar = g_lastSignalBar;
         g_state = STATE_DONE;
         break;

      case STATE_DONE:
         break;
   }

   UpdateDashboard();
}
//+------------------------------------------------------------------+
int CountOurTrades()
{
   int count = 0;
   for(int i = 0; i < OrdersTotal(); i++)
   {
      if(!OrderSelect(i, SELECT_BY_POS, MODE_TRADES)) continue;
      if(OrderMagicNumber() == MagicNumber && (OrderType() == OP_BUY || OrderType() == OP_SELL))
         count++;
   }
   return count;
}
//+------------------------------------------------------------------+
void CloseAllTrades()
{
   for(int i = OrdersTotal() - 1; i >= 0; i--)
   {
      if(!OrderSelect(i, SELECT_BY_POS, MODE_TRADES)) continue;
      if(OrderMagicNumber() != MagicNumber) continue;
      if(OrderType() != OP_BUY && OrderType() != OP_SELL) continue;

      string sym  = OrderSymbol();
      int    cmd  = OrderType();
      double lots = OrderLots();
      int    tk   = OrderTicket();

      RefreshRates();
      double px = (cmd == OP_BUY) ? MarketInfo(sym, MODE_BID) : MarketInfo(sym, MODE_ASK);
      bool ok = OrderClose(tk, lots, px, 30, clrGray);
      if(!ok && PrintDebug)
         Print("Close failed: ", sym, " ticket=", tk, " err=", GetLastError());
   }
}
//+------------------------------------------------------------------+
double CurrentLotsForSymbol(string sym)
{
   double lots = BaseLots;

   if(UseCompounding && EquityStep > 0.0)
      lots = BaseLots * (AccountEquity() / EquityStep);

   return NormaliseLotsForSymbol(sym, lots);
}
//+------------------------------------------------------------------+
void OpenTodaysTrades()
{
   for(int p = 0; p < g_pairCount; p++)
   {
      string sym = g_pairList[p];
      if(!SymbolSelect(sym, true)) continue;

      double slope = GetLegacySlope(sym);
      if(slope == EMPTY_VALUE) continue;

      double lots = CurrentLotsForSymbol(sym);

      if(slope > Threshold)
         OpenTrade(sym, OP_SELL, lots, slope);
      else if(slope < -Threshold)
         OpenTrade(sym, OP_BUY, lots, slope);
   }
}
//+------------------------------------------------------------------+
void OpenTrade(string sym, int cmd, double lots, double slope)
{
   RefreshRates();
   double px = (cmd == OP_BUY) ? MarketInfo(sym, MODE_ASK) : MarketInfo(sym, MODE_BID);

   int ticket = OrderSend(sym, cmd, lots, px, 30, 0, 0, "BeastTop10Simple", MagicNumber, 0,
                          cmd == OP_BUY ? clrDodgerBlue : clrOrangeRed);

   if(ticket <= 0 && PrintDebug)
      Print("Open failed ", sym, " lots=", DoubleToStr(lots,2), " slope=", DoubleToStr(slope,4), " err=", GetLastError());
}
//+------------------------------------------------------------------+
// Current NB-TMA used in JK TMA Strength Dashboard
// t0 = CalcTMA_NB(shift=1, period=20)
// t1 = CalcTMA_NB(shift=2, period=20)
// slope = (t0 - t1) / (ATR(100, shift 11) / 10)
//+------------------------------------------------------------------+
double NB_TMA(string sym, ENUM_TIMEFRAMES tf, int centerShift, int ignoredTodayShift, int period)
{
   int bars = iBars(sym, tf);
   if(bars <= 0) return EMPTY_VALUE;
   if(centerShift < 0 || centerShift + 1 >= bars) return EMPTY_VALUE;
   if(centerShift + period >= bars) return EMPTY_VALUE;

   double centerPrice = iClose(sym, tf, centerShift);
   if(centerPrice == 0.0) return EMPTY_VALUE;

   double sum = centerPrice * (period + 1);
   double weight_sum = (period + 1);

   for(int j = 1; j <= period; j++)
   {
      double w = (period + 1 - j);

      double olderPrice = iClose(sym, tf, centerShift + j);
      if(olderPrice == 0.0) return EMPTY_VALUE;
      sum += w * olderPrice;
      weight_sum += w;

      if(j <= centerShift)
      {
         double newerPrice = iClose(sym, tf, centerShift - j);
         if(newerPrice == 0.0) return EMPTY_VALUE;
         sum += w * newerPrice;
         weight_sum += w;
      }
   }

   return(sum / weight_sum);
}
//+------------------------------------------------------------------+
double GetLegacySlope(string sym)
{
   int needBars = TmaPeriod + AtrPeriod + 20;
   if(iBars(sym, SignalTimeframe) < needBars) return EMPTY_VALUE;

   double t0 = NB_TMA(sym, SignalTimeframe, 1, 0, TmaPeriod);
   double t1 = NB_TMA(sym, SignalTimeframe, 2, 0, TmaPeriod);
   if(t0 == EMPTY_VALUE || t1 == EMPTY_VALUE) return EMPTY_VALUE;

   double a = iATR(sym, SignalTimeframe, AtrPeriod, 11);
   if(a <= 0.0) return EMPTY_VALUE;

   double slope = (t0 - t1) / (a / 10.0);

   if(PrintDebug)
      Print(sym, " | t0=", DoubleToStr(t0,5), " t1=", DoubleToStr(t1,5),
            " atr11=", DoubleToStr(a,5), " slope=", DoubleToStr(slope,4));

   return slope;
}
//+------------------------------------------------------------------+
double NormaliseLotsForSymbol(string sym, double lots)
{
   double minLot  = MarketInfo(sym, MODE_MINLOT);
   double maxLot  = MarketInfo(sym, MODE_MAXLOT);
   double lotStep = MarketInfo(sym, MODE_LOTSTEP);
   if(lotStep <= 0.0) lotStep = 0.01;

   lots = MathFloor(lots / lotStep) * lotStep;
   lots = MathMax(minLot, MathMin(maxLot, lots));
   return NormalizeDouble(lots, 2);
}
//+------------------------------------------------------------------+
void ParsePairs()
{
   string raw = Pairs;
   StringTrimLeft(raw); StringTrimRight(raw);
   g_pairCount = 0;
   ArrayResize(g_pairList, 32);

   string token = "";
   for(int c = 0; c <= StringLen(raw); c++)
   {
      string ch = (c < StringLen(raw)) ? StringSubstr(raw, c, 1) : ",";
      if(ch == ",")
      {
         StringTrimLeft(token); StringTrimRight(token);
         if(StringLen(token) > 0)
         {
            g_pairList[g_pairCount] = token;
            g_pairCount++;
         }
         token = "";
      }
      else token += ch;
   }
}
//+------------------------------------------------------------------+
string TfToStr(ENUM_TIMEFRAMES tf)
{
   switch(tf)
   {
      case PERIOD_M15: return "M15";
      case PERIOD_M30: return "M30";
      case PERIOD_H1:  return "H1";
      case PERIOD_H4:  return "H4";
      case PERIOD_D1:  return "D1";
   }
   return IntegerToString((int)tf);
}
//+------------------------------------------------------------------+
void UpdateDashboard()
{
   if(!ShowDashboard)
   {
      ClearDashObjects();
      Comment("");
      return;
   }

   DashRow bull[], range[], bear[];
   ArrayResize(bull, 0);
   ArrayResize(range, 0);
   ArrayResize(bear, 0);

   for(int p = 0; p < g_pairCount; p++)
   {
      string sym = g_pairList[p];
      double slope = GetLegacySlope(sym);
      if(slope == EMPTY_VALUE) continue;

      DashRow r;
      r.sym = sym;
      r.slope = slope;

      if(slope > Threshold)
      {
         int n = ArraySize(bull);
         ArrayResize(bull, n + 1);
         bull[n] = r;
      }
      else if(slope < -Threshold)
      {
         int n2 = ArraySize(bear);
         ArrayResize(bear, n2 + 1);
         bear[n2] = r;
      }
      else
      {
         int n3 = ArraySize(range);
         ArrayResize(range, n3 + 1);
         range[n3] = r;
      }
   }

   SortDashDesc(bull);
   SortDashAbsDesc(range);
   SortDashAsc(bear);

   ClearDashObjects();
   Comment("");

   int x1 = 12;
   int x2 = 220;
   int x3 = 430;
   int y0 = 14;
   int maxRows = MathMax(ArraySize(bull), MathMax(ArraySize(range), ArraySize(bear))) + 2;

   DrawDashBucket("BULL", x1, y0, StringFormat("> %.2f  Bullish", Threshold), bull, clrLime);
   DrawDashBucket("RANGE", x2, y0, StringFormat("±%.2f  Ranging", Threshold), range, clrGold);
   DrawDashBucket("BEAR", x3, y0, StringFormat("< -%.2f Bearish", Threshold), bear, clrTomato);

   DrawDashSeparators(x2 - 22, x3 - 22, y0, maxRows);
}
//+------------------------------------------------------------------++
