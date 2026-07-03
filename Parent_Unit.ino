#include <esp_now.h>      //EPS-NOW通信用のライブラリ
#include <WiFi.h>         //WiFiを使うためのライブラリ
#include <Wire.h>         //LCDを使うためのライブラリ
#include "esp_wifi.h"     //親機のチャンネルを強制固定するためのライブラリ

//設定項目
#define I2C_ADDR 0x3E     //AE-AQM1620AwpのI2Cアドレス
const int buzzerPin = 25;   //ブザーをつなぐピンを25番

//強度のしきい値
const int borderLine = -50; //軽い警告【約2m離れたときの数値】
const int dangerLine = -70; //強い警告

//猶予時間(ミリ秒)
const int alertDelay  = 5000;            //5秒間強度が低いままなら発報
const int lostDelay = 7000;              //7秒間通信が途絶えたらロスト発報
const int recoveryDelay = 2000;         //2秒間安全圏が続かないと復帰したとみなさない

//ブザー音の設定
const int freq = 2000;      //音の高さ2000Hz
const int resolution = 8;  //音の細かさの設定

//状態管理変数
int kyoudo = 0;                             //現在の接続強度を保存
unsigned long lastRecvTime = 0;             //最後に電波を受信した時刻を保存
unsigned long lowSignalStartTime = 0;       //低強度(離れた状態)が始まった時刻を記録する変数
unsigned long recoveryStartTime = 0;        //安全圏に戻った時間を保存     
String lastMessage = "";                    //液晶2段目前回の文字を記憶し、無駄な再書き込みを防ぐ
unsigned long lastLcdUpdataTime = 0;        //液晶1段目を最後に書き終えた時刻を保存
unsigned long i2cErrorStartTime = 0;        //液晶への通信エラーが発生し始めた時間を保存(フリーズ監視用タイマ)

//構造体の定義(子機から届くデータの入れ物)
typedef struct struct_message{
  int buttonStatus;     //0:何もなし　1:ボタンが押された
}struct_message;
struct_message incomingData;    //届いたデータを入れる変数

uint8_t childAddress[] = {0x10, 0x06, 0x1C, 0x40, 0xB5, 0x14};      //子機のMACアドレス
struct_message myData;                     //親機から子機へ定期送信して子機に強度をはからせるためのデータを入れる箱

/*---------------------------------------液晶操作関数-----------------------------------------*/
//【フリーズ自動復旧】液晶がノイズや電圧ドロップでフリーズしたときに裏で強制リセットをかけ復帰させる関数
void checkAndRecverI2C(){
  //送信エラーが発生し(i2cErrorStartTimeが0以外)その状態が1秒(1000ms)以上続いていたら
  if(i2cErrorStartTime != 0 && (millis() - i2cErrorStartTime > 1000)){
    Serial.println("【警告】液晶通信のフリーズを検知、自動復旧します");
    Wire.end();               //液晶との通信線ハングアップ状態ごと完全にシャットダウン
    delay(10);                //回線が落ち着くまで10ミリ待機
    Wire.begin(21,22);        //再度21番22番ピンでI2C通信を初期化して接続を再開
    Wire.setClock(100000);    //液晶の本来のノイズに強く安定した速度(100kHz)に通信クロックを設定
    Wire.setTimeOut(20);      //液晶が応答しなくても最大20msであきらめて処理を次に進める

    //液晶画面のハードウェア内部を完全にリフレッシュするためのリセットコマンド群を送信
    lcd_command(0x38);  lcd_command(0x39);  lcd_command(0x14);
    lcd_command(0x73);  lcd_command(0x56);  lcd_command(0x6C);
    delay(200);                                                   //液晶が立ち上がるのを少し待つ
    lcd_command(0x38);  lcd_command(0x0C);  lcd_command(0x01);    //画面をまっさらにクリア

    //画面の初期化により文字が消えたため、次回のループで強制的に現在の最新文字を再度表示
    lastMessage = "";
    lastLcdUpdataTime = 0;
   }
}

//液晶へコマンドを送信する関数
void lcd_command(uint8_t command){
  Wire.beginTransmission(I2C_ADDR);               //液晶のアドレスを指定して通信スタート
  Wire.write(0x00);                               //コマンド指定
  Wire.write(command);                            //コマンドの本体データを送る
  //送信を完了し液晶からの返事(ACK)を確認。0以外なら液晶が返事をしていない(エラー)
  if(Wire.endTransmission() != 0){                
    //エラーが起き、エラータイマが動いてなければi2cErrorStartTimeへ時刻を記録
    if(i2cErrorStartTime == 0){                   //0をタイマが動いていない
      i2cErrorStartTime = millis();               //エラー開始時間を記録
    }
    return;                            //固まるのを防ぐためこれ以上液晶の操作はせs図関数を抜ける
  }
  i2cErrorStartTime = 0;                          //正常に通信で来たらエラータイマをリセット
  delay(5);                                       //液晶が支持を処理するための猶予時間5ミリ待つ
}

//液晶へ文字データを送信する関数
  void lcd_data(uint8_t data){
    Wire.beginTransmission(I2C_ADDR);             //液晶への通信スタート
    Wire.write(0x40);                             //今から送るのは画面に表示する文字データという合図を送る
    Wire.write(data);                             //文字のデータ本体を送る
    Wire.endTransmission();                       //送信完了
  }

//液晶に文字列を表示する関数
//送られてきた文字列を1文字ずつ分解して連続で画面に表示
void lcd_print(const char *str){
  while(*str){                                    //文字列の末尾に到達するまでループ　　*str＝0で条件一致終了
    lcd_data(*str++);                             //1文字液晶に送るごとに次の文字へとアドレスを1つ進める
  }
}

//液晶の初期化関数を新規作成
void lcd_init(){
  Wire.begin(21,22);                               //SDAを21番SCLを22番ピンに指定しI2C通信を開始
  delay(100);                                      //通信回路が安定するまで100ミリ秒待つ
  //液晶の取説に指定されている初期設定用のコマンド群を順番に送信
  lcd_command(0x38);  lcd_command(0x39);  lcd_command(0x14);
  lcd_command(0x73);  lcd_command(0x56);  lcd_command(0x6C);
  delay(200);                                       //液晶内部の初期化処理を200ミリ待つ
  lcd_command(0x38);  lcd_command(0x0C);  lcd_command(0x01);  //画面をオンにしてクリア
  delay(20);                                        //完全に準備が整うまで20ミリ待つ
}


//【重要】子機から電波を物理的に受信した瞬間に裏で呼び出される関数
void OnDataRecv(const esp_now_recv_info_t * info, const uint8_t * data, int len){
 //届いたバイナリデータを一番上で設計した構造体の形にそっくりそのままコピー
  memcpy(&incomingData, data, sizeof(incomingData));
  //受信時の接続強度の保存
  kyoudo = info->rx_ctrl->rssi;
  lastRecvTime = millis();               //子機の電波を受信した時間を更新

  //シリアルモニタにりたるタイムで接続強度を表示
  Serial.print("電波強度(RSSI):");      //デバッグ用
  Serial.println(kyoudo);

}
/*********************************セットアップ*********************************/
//電源が入ったとき、又はリセットボタンが押されたときに実行される関数
void setup() {

  Serial.begin(115200);          //シリアル通信速度を115200に設定 
  setCpuFrequencyMhz(80);        //CPUの動作速度を標準の240Mhzから80MHzに落としノイズと消費電力を激減させる
  lcd_init();                                 //液晶の準備
  ledcAttach(buzzerPin, freq, resolution);    //ブザーの準備
  
  Wire.setClock(100000);                      //液晶通信の速度を100kHzにロック
  Wire.setTimeOut(20);                        //通信フリーズ防止用のタイムアウト20ms
   
  WiFi.mode(WIFI_STA);                        //WiFiをステーションモードに設定
  WiFi.setTxPower(WIFI_POWER_11dBm);          //無駄な電力を放出して回路にノイズを発生させないよう出力を抑える

  //【双方向通信対策】親機と子機の電波周波をチャンネル1で完全に一致させる
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  WiFi.setSleep(false);                       //WiFino省電力スリープ機能をオフにし子機からの電波を24時間受信可能に
  WiFi.disconnect();                          //過去のWiFiルーターの情報を念のためリセット

  //親機のMACアドレスをシリアルモニタへ表示
  Serial.print("子機に書き込む親機のMACアドレス:");
  Serial.println(WiFi.macAddress());
  
  //ESP-NOWの初期化
  if(esp_now_init() != ESP_OK){           
    Serial.println("ESP-NOW初期化失敗");
    delay(5000);
    ESP.restart();        //本体リセット(再起動)
    return;
  }

  //初期化できたらOnDataRecvを自動起動
  esp_now_register_recv_cb(OnDataRecv);

  //【双方向通信対策】loop内で子機に定期電波を送るために子機の番号を登録
  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));             //中身をクリア
  memcpy(peerInfo.peer_addr, childAddress, 6);        //通信相手の欄に子機のアドレスを書き込む
  peerInfo.channel = 1;                               //チャンネル1を指定
  peerInfo.encrypt = false;                           //暗号化は使わない

  //親機を通信相手として登録
  if(esp_now_add_peer(&peerInfo) != ESP_OK){
    Serial.println("子機のペアリング失敗");
    delay(5000);
    ESP.restart();        //本体リセット(再起動)
    return;

  }else{
    Serial.println("子機のペアリング登録成功");
  }
  Serial.println("親機：受信待ちの状態です");
}

/*********************************ループ*********************************/
//セットアップが終わったら電源が切れるまで実行
void loop() {  
  unsigned long now = millis();           //起動してからの現在時刻
  char lcd_buf[20];                       //表示用バッファ

  checkAndRecverI2C();                    //液晶がフリーズしていないか毎ループチェック＆自動復帰

  //【双方向対応の定期送信】子機に電波をはからせるきっかけをつくるため、親機からもデータを送信
  myData.buttonStatus = 0;                //親機はボタンを押していないので通常データセット【今後の拡張用】
  esp_now_send(childAddress, (uint8_t*) &myData, sizeof(myData)); //子機のアドレスに向けでデータ送信

  //Lost判定の作成true/false
  //子機から一度も受信していないもしくは最後に受信した時間から7秒以上たっていたらロストと判断
  bool isLost = (lastRecvTime == 0 || (now - lastRecvTime > lostDelay));

 // 1段目：電波強度の表示(毎ループ書き換えると通信ノイズで液晶がフリーズするため500msに1回だけ)
  if(now - lastLcdUpdataTime > 500){
    lcd_command(0x80);                                //液晶の1行目の先頭にカーソル位置を移動
    if (lastRecvTime == 0) {
      lcd_print("SEARCHING...   ");                   //子機からの電波を一度も受けていなければ検索中と表示
    }else{
      //現在の最新電波強度を表示
      sprintf(lcd_buf, "RSSI: %d dBm    ", kyoudo);
      lcd_print(lcd_buf);
    }
    lastLcdUpdataTime = now;                          //1行目を更新した現時刻を保存し次の500msに備える
  }

  // 2段目：警告判定 
  String currentMessage = "";                   //子のループ内で液晶の2段目に表示すべき文字を入れておくための空変数

  // 【判定1】通信途絶判定 (最優先)ロストの時発報
  if (isLost) {
    currentMessage = "LOST:ALERT!!   ";         //文字を保存
    //500msおきにブザーをぴっぴー、ぴっぴーとゆっくり鳴らすためのON/OFF計算
    if ((now / 500) % 2 == 0) ledcWriteTone(buzzerPin, freq);     //割り切れる時間は音を鳴らす
    else ledcWriteTone(buzzerPin, 0);                             //割り切れない時間は音を止める
  }
    
  // 【判定2】電波強度がしきい値-50以下の時
  else if (kyoudo <= borderLine) {
    recoveryStartTime = 0;                      // 安全圏タイマーをリセット
    // まだカウントが始まっていなければ、今この瞬間をスタートにする
    if (lowSignalStartTime == 0) lowSignalStartTime = now;
    
    unsigned long duration = now - lowSignalStartTime;      //離れ始めてから何秒経過したか
    //【判定2-A】離れ始めてから5秒以上経過したら警告アラート
    //バッグの中や壁などの遮へい物を考慮しすぐには鳴らさない
    if (duration >= alertDelay) {
      //強度が危険圏の-70以下まで離れていたら最終通告の警報アラート
      if (kyoudo <= dangerLine) {
        currentMessage = "DANGER: TOO FAR ";    //文字を保存
        if ((now / 400) % 2 == 0) ledcWriteTone(buzzerPin, freq);
        else ledcWriteTone(buzzerPin, 0);
      } 
      //-50～-69の間の緩やかな離れの場合は注意(CAUTION)を通知
      else {
        currentMessage = "CAUTION: AWAY   ";  // 表示する文字をセット
        Serial.println("電波強度低下(5秒以上経過)");
        //400msごとにぴーぴーと警告音を鳴らす
         //100msおきに3拍子のテンポでぴぴぴっぴぴぴっと強めので高い音を発報
        if ((now / 100) % 3 == 0) ledcWriteTone(buzzerPin, freq + 500);   //500Hz高くして音を鳴らす
        else ledcWriteTone(buzzerPin, 0);
 
      }
    
    }
    //【判定2-B】離れ始めてからまだ5秒たってない間は1秒ごとにカウントダウンの数字を表示 
    else {
      // 5秒経過するまでのカウントダウン
      int remaining = (alertDelay - duration) / 1000;   //残り何秒あるかをミリから秒へ
      sprintf(lcd_buf, "CHECKING...%ds ", remaining);   //文字列を構成
      currentMessage = String(lcd_buf);                 //組み立てた文字を表示
      ledcWriteTone(buzzerPin, 0);                      //猶予期間中のためブザーは鳴らさない
    }
  }

  // 【判定3】電波強度が安全圏-49以上の時
  else {
    ledcWriteTone(buzzerPin,0);                          //安全圏に戻ったのでブザーを即止める
    //直前までlowSignalStartTimeが動いていたなら様子見として戻ってきたタイミングで開始
    if (lowSignalStartTime != 0) {
      if (recoveryStartTime == 0) recoveryStartTime = now;  //戻ってきたら様子見でタイマースタート
      //様子見を開始してから2秒以上安全圏をキープで来たらタイマーをリセット(最終確認)
      if (now - recoveryStartTime >= recoveryDelay) {
        lowSignalStartTime = 0;                           //猶予時間をはかるタイマーをリセット
        recoveryStartTime = 0;                            //様子見のタイマーをリセット
        currentMessage = "SAFE ZONE      ";
      }
      //まだ戻ってきてから2秒たっていない間は復帰中を表示
      else{
        currentMessage = "RECOVERING...  ";
      }
    }
    //直前も安全圏にいた場合はセーフゾーンの表示
    else{
      currentMessage = "SAFE ZONE      ";
      ledcWriteTone(buzzerPin, 0);
    }
  }

  //【液晶フリーズ防止ロジック】
  //今の2段目の文字(currtntMessage)が前回表示した文字(lastmessage)と
  //変化した瞬間だけ液晶画面の2段目にデータ送信して書き換える
  //これにより同じ文字を毎秒何百回も液晶に送り続けて通信線がバグるのを防ぐ
  if (currentMessage != lastMessage && currentMessage != "") {
    lcd_command(0xC0);        //液晶の2行目の先頭にカーソル移動
    // 液晶は16文字なので、足りない部分をスペースで埋める処理
    //過去の文字が右端に残るバグを防ぐ
    while(currentMessage.length() < 16) {
      currentMessage += " ";
    }
    lcd_print(currentMessage.c_str());      //16文字ぴったりの文字データを液晶に送信して上書き
    lastMessage = currentMessage;           //今回表示した文字を前回の文字として記録(次回ループ用)
    }
  delay(100);
}