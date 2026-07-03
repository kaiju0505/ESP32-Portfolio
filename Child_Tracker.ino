#include <esp_now.h>      // ESP-NOW通信用のライブラリ
#include <WiFi.h>         // WiFiを使うためのライブラリ
#include <Wire.h>         // LCDを使うためのライブラリ
#include "esp_wifi.h"     // WiFi詳細設定用

// 設定項目
#define I2C_ADDR 0x3E     // AE-AWM1620AwpのI2Cアドレス
const int buzzerPin = 25;  // 子機ブザー
const int ledR      = 26;  // RGB LED 赤
const int ledG      = 27;  // RGB LED 緑
const int ledB      = 14;  // RGB LED 青
float hue           = 0;   // 虹色演出用の変数
const int buttonPin = 32;  // 遊べるボタン

// 強度のしきい値
const int borderLine = -50;   // 注意し始める距離(電波強度)
const int dangerLine = -70;   // 危険を感じる距離(電波強度)

// 猶予時間(ミリ秒)
const int alertDelay = 5000;   // 5秒間強度が低いままなら発報
const int lostDelay = 7000;    // 7秒間通信が途絶えたらロスト発報

// ブザーの音設定
const int softFreq    = 800;          // 低めの優しい音
const int resolution  = 8;            // 音の細かさ設定

// 状態管理変数
int kyoudo = 0;                            // 親機との現在の最新電波強度
unsigned long lastRecvTime = 0;            // 親機から最後に連絡が来た時刻
unsigned long lowSignalStartTime = 0;      // 電波が弱まってからの計測用タイマー
//volatile bool dataReceivedFlag = false;  // データを受信したかどうかを一時的に記憶しておくためのフラグ

// 液晶フリーズ自動復旧用変数
String lastLine1Message = "";               // 液晶1段目に前回表示した文字を記憶し、残留を防ぐ
String lastLine2Message = "";               // 液晶2段目に前回表示した文字を記憶し、残留を防ぐ
String currentLine1Message = "";            // 1段目の最新メッセージを入れる箱
String currentLine2Message = "";            // 2段目の最新メッセージを入れる箱
unsigned long lastLcdUpdataTime = 0;        // 液晶1段目(RSSI表示)を最後に書き換えた時刻を記憶する
unsigned long i2cErrorStartTime = 0;        // 液晶への通信エラーが発生し始めた時刻を記憶するフリーズ監視用タイマー

//親機のMACアドレス、親機にピンポイントで送る
uint8_t broadcastAddress[] = {0xD0, 0xEF, 0x76, 0x05, 0x71, 0xFC};

//構造体の定義(親機に送るデータの入れ物)ボタン遊び用、親機に送ることができる
typedef struct struct_message{
  int buttonStatus;               //0:何もなし　1:ボタンが押された
}struct_message;
struct_message myData;            //自分の送信データ用

//前方宣言
void lcd_command(uint8_t command);

/*-------------------------液晶操作関数------------------------*/
//【フリーズ自動復旧】液晶がノイズでバグを起こしたときに強制リセットをかけて復帰(親機と同じ)
void checkAndRecverI2C(){
  if(i2cErrorStartTime != 0 && (millis() - i2cErrorStartTime > 1000)){
    Serial.println("【警告】液晶通信のフリーズを検知、自動復帰します");
    Wire.end();               //一度回線をハングアップ状態事完全に切断
    delay(10);                //回線が落ち着くまで10ms待機
    Wire.begin(21,22);        //再度21番22番ピンでI2C通信を開きなおす
    Wire.setClock(100000);    //安定した100kHzに通信速度を設定
    Wire.setTimeOut(20);      //最大20msで反応をあきらめフリーズを防止

    //液晶を再起動コマンドで起こす
    lcd_command(0x38);  lcd_command(0x39);  lcd_command(0x14);
    lcd_command(0x73);  lcd_command(0x56);  lcd_command(0x6C);
    delay(200);                 //液晶が立ち上がるのを待つ
    lcd_command(0x38);  lcd_command(0x0C);  lcd_command(0x01);    //画面クリア

    //画面が消えたので、次回ループで強制的に現在の最新文字を描画
    lastLine1Message = "";
    lastLcdUpdataTime = 0;
    i2cErrorStartTime = 0;
  }
}
//液晶へコマンドを送信
void lcd_command(uint8_t command){
  Wire.beginTransmission(I2C_ADDR);     //液晶のアドレスを指定して通信開始
  Wire.write(0x00);                     //コマンド指定
  Wire.write(command);                  //指示の内容送信
  if(Wire.endTransmission() != 0){
    //通信エラーが起きたら、まだタイマーが動いてなければエラー開始時刻を記録
    if(i2cErrorStartTime == 0) i2cErrorStartTime = millis(); 
    return;     //処理を抜けて全体のフリーズを回避
  }
  i2cErrorStartTime = 0;      //成功したらタイマーをリセット
  delay(5);                   //液晶が処理を終えるまで5ms待つ
}
//液晶へ文字データを送信
void lcd_data(uint8_t data){  
  Wire.beginTransmission(I2C_ADDR);   //液晶への通信開始
  Wire.write(0x40);                   //今から文字データを送りますという合図
  Wire.write(data);                   //文字のデータを送信
  Wire.endTransmission();             //送信完了
  }

//液晶に文字列を表示(1文字ずつ分解して液晶へ表示)
void lcd_print(const char *str){
  while(*str){                        //文字列の終わりに到達するまでループ
    lcd_data(*str++);                //1文字液晶に送るごとに次の文字へとアドレスを1つ進める
  }
}
//液晶の初期化関数
void lcd_init(){
  Wire.begin(21,22);
  delay(100);
  lcd_command(0x38);  lcd_command(0x39);  lcd_command(0x14);
  lcd_command(0x73);  lcd_command(0x56);  lcd_command(0x6C);
  delay(200);
  lcd_command(0x38);  lcd_command(0x0C);  lcd_command(0x01);
  delay(20);  
}

//液晶のカーソル位置指定関数
void lcd_setCursor(unsigned char col, unsigned char row){
  unsigned char addr;

  if(row == 0){
    addr = 0x80 + col;    //1行目：0x80~0x87      
  }else{
    addr = 0xC0 + col;    //2行目：0xC0~0xC7
  }
  lcd_command(addr);
}

/*-------------------------------------------------------------------*/

/*-----------------------LED・音の関数----------------------*/
//LEDの明るさを個別指定(0～255)
void setRGB(int r, int g, int b){
  analogWrite(ledR, r);
  analogWrite(ledG, g);
  analogWrite(ledB, b);
}

///フルカラーLEDを滑らかに虹色グラデーションさせる
void rainbowLite(){
  //サイン波を使って滑らかに赤・緑・青させる
  int r = sin(hue) * 127 + 128;         //赤の波
  int g = sin(hue + 2.09) * 127 + 128;  //緑の波(少しずらす)
  int b = sin(hue + 4.18) * 127 + 128;  //青の波(さらにずらす)
  setRGB(r, g, b);   //計算したら色で光らせる
  hue += 0.20;       //変化の速さ(小さくするとゆっくりになる)
}
///子機用の警告音
void playSoftTone(int times, int duration){
  for(int i = 0; i < times; i++){
    ledcWriteTone(buzzerPin, softFreq);
    delay(duration);
    ledcWriteTone(buzzerPin, 0);
    delay(duration);
  }
}

//親機から電波が届いたときに強度を測定する(最優先関数)
void OnDataRecv(const esp_now_recv_info_t * info, const uint8_t * data, int len){
  kyoudo = info ->rx_ctrl->rssi;    //電波強度(RSSI)を取得
  lastRecvTime = millis();

  Serial.print("電波強度(RSSI):");
  Serial.println(kyoudo);

}
/*----------------------------ステータスバー---------------------------*/
String getStatusBarString(int rssi){
  int bars = 0;
  if(rssi > -45)        bars = 5;     //[■ ■ ■ ■ ■]
  else if(rssi > -55)   bars = 4;     //[■ ■ ■ ■]
  else if(rssi > -65)   bars = 3;     //[■ ■ ■]
  else if(rssi > -75)   bars = 2;     //[■ ■]
  else if(rssi > -85)   bars = 1;     //[■]
  else                  bars = 0;     //[ ]

  String result = "[";
  for(int i = 0; i < 5; i++){
    if(i < bars)  result += "O";
    else          result += " ";  //空白
    }
    result += "]";
    return result;   
}

/*********************************セットアップ*********************************/
void setup() {

  Serial.begin(115200);           //シリアル通信速度を115200に設定
  setCpuFrequencyMhz(80);         //CPUの動作速度を標準の240Mhzから80MHzに落としノイズと消費電力を激減させる
  lcd_init();                                      //液晶の準備
  pinMode(buttonPin, INPUT_PULLUP);                //ボタンの準備
  ledcAttach(buzzerPin, softFreq, resolution);     //ブザーの準備

  Wire.setClock(100000);                      //液晶通信の速度を100kHzにロック
  Wire.setTimeOut(20);                        //通信フリーズ防止用のタイムアウト20ms
 
  WiFi.mode(WIFI_STA);                             //WiFiをステーションモードに設定
  WiFi.setTxPower(WIFI_POWER_11dBm);               //無駄な電力を放出して回路にノイズを発生させないよう出力を抑える

  //【双方向通信対策】親機と子機の電波周波をチャンネル1で完全に一致させる
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);   //親機と通信チャンネルを合わせて電波がずれないように設定

  WiFi.setSleep(false);                             //WiFino省電力スリープ機能をオフにし子機からの電波を24時間受信可能に
  WiFi.disconnect();                                //過去のWiFiルーターの情報を念のためリセット
  
  //ESP-NOWの初期化
  if(esp_now_init() != ESP_OK){           
    Serial.println("ESP-NOW初期化失敗");
    delay(5000);
    ESP.restart();        //本体リセット(再起動)
    return;
  }

  //受信したときに強度を図る設定
    esp_now_register_recv_cb(OnDataRecv);

  //【双方向通信対策】loop内で親機に定期電波を送るために親機の番号を登録
  esp_now_peer_info_t peerInfo;
  //【重要】設定を一度クリアする
  memset(&peerInfo, 0, sizeof(peerInfo));                //中身をクリア 
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);       //通信相手の欄に子機のアドレスを書き込む
  peerInfo.channel = 1;                                  //親機と一致させるために1
  peerInfo.encrypt = false;                              //暗号化を明示的は使わない

  //親機を通信相手として登録
  if(esp_now_add_peer(&peerInfo) != ESP_OK){
    Serial.println("ペアリング失敗");
    delay(5000);
    ESP.restart();        //本体リセット(再起動)
    return;
  }
  Serial.println("子機：双方向受信モードで正常起動しました");
}

/*********************************ループ*********************************/
void loop() {
  unsigned long now = millis();             //起動してからの現在時刻
  char lcd_buf[20];                         //表示用バッファ

/*
  if(dataReceivedFlag == true){
    lastRecvTime = millis();
    dataReceivedFlag = false;
  }
*/
  checkAndRecverI2C();                      // 液晶がノイズでバグっていないか毎ループ監視

 //【双方向対応の定期送信】親機に電波をはからせるきっかけをつくるため、子機からもデータを送信
  myData.buttonStatus = (digitalRead(buttonPin) == LOW) ? 1 : 0;
  esp_now_send(broadcastAddress, (uint8_t *) &myData, sizeof(myData));

  //Lost判定の作成true/false
  //子機から一度も受信していないもしくは最後に受信した時間から7秒以上たっていたらロストと判断
  bool isLost = (lastRecvTime == 0 || (now - lastRecvTime > lostDelay));

 
  // 【最優先】通信切れ（LOST）のとき
  if(isLost){
    currentLine1Message = " x_x;; LOST!!  ";    // 1行目：警告の顔文字
    currentLine2Message = "LOST CONNECTION ";   // 2行目：通信切れメッセージ
    ledcWriteTone(buzzerPin, 400);              // ブザー鳴らす
    setRGB(0, 255, 255);                        // LED：真っ赤
    lowSignalStartTime = 0;                     // タイマーリセット
  }
  // 【電波を探している間】（初期状態）
  else if(lastRecvTime == 0){
    currentLine1Message = "SEARCHING...    ";
    currentLine2Message = "PLEASE WAIT...  ";
    int pulse = (sin(millis() / 200.0) + 1.0) * 60 + 10;
    setRGB(255, 255 - pulse, 255 - pulse);         // LED：水色明滅
    ledcWriteTone(buzzerPin, 0);
    lowSignalStartTime = 0;                       // タイマーリセット
  }
  // 【正常通信中】電波の強さに応じて分岐
  else {
    // ステータスバー [OOOOO] を最初に左側へセットする
    currentLine1Message = getStatusBarString(kyoudo);

    // ------------------------------------------
    // パターンA：【安全圏】（十分に近くにいるとき）
    // ------------------------------------------
    if(kyoudo > borderLine){
      currentLine1Message += " *^v^* OK ";
      currentLine2Message =  "SAFE WITH YOU ";
      rainbowLite();                             // LED：虹色
      ledcWriteTone(buzzerPin, 0);
      lowSignalStartTime = 0;                    // ★安全圏のみタイマーをリセット
    }
    // ------------------------------------------
    // パターンB＆C：【警告・危険圏】（離れ始めたとき）
    // ------------------------------------------
    else {
      // 離れ始めてからの時間を計測（ここでタイマーを開始・維持する）
      if(lowSignalStartTime == 0) lowSignalStartTime = now;
      unsigned long duration = now - lowSignalStartTime;

      // LEDの色と顔文字の判定（時間はカウントしつつ、電波強度で見た目だけ分ける）
      if(kyoudo > dangerLine){
        currentLine1Message += " o_o;; !! ";
        setRGB(125, 255, 0);                     // LED：あかるいむらさき
      } else {
        currentLine1Message += " ToT; NO! ";
        setRGB(0, 255, 255);                     // LED：赤
      }

      // --- タイマーの判定と発報処理 ---
      if(duration >= alertDelay){
        // 5秒以上経過したら「MAMA! PAPA!」を発報
        currentLine2Message = "MAMA! PAPA!    ";
       playSoftTone(1, 200);
      }else{
        // 5秒未満のときはカウントダウンを表示
        int rem = (alertDelay - duration) / 1000;
        sprintf(lcd_buf, "WAIT... %ds", rem);
        currentLine2Message = String(lcd_buf);
        ledcWriteTone(buzzerPin, 0);
      }
    }
  }
 
  // 1行目の表示処理（500msに1回チェック）
  if (now - lastLcdUpdataTime > 500) {
    while (currentLine1Message.length() < 16) {
      currentLine1Message += " ";
    }
    if (currentLine1Message != lastLine1Message) {
      lcd_command(0x80); 
      lcd_print(currentLine1Message.c_str());
      lastLine1Message = currentLine1Message;
    }
      // 2行目の表示処理
    while (currentLine2Message.length() < 16) {
      currentLine2Message += " ";
    }
    if (currentLine2Message != lastLine2Message) {
      lcd_command(0xC0); 
      lcd_print(currentLine2Message.c_str());
      lastLine2Message = currentLine2Message; 
    }
      lastLcdUpdataTime = now;
  }
 
  // ボタンが押されたとき(遊びのベース)
  if(myData.buttonStatus == 1){
    Serial.println("ボタンが押されました！");
  }

  delay(100); 
}