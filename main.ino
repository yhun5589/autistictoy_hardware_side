#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Irisoled.h>
#include <FS.h>
#include <SPIFFS.h> // Internal flash file system engine
#include <WiFi.h>
#include <WebServer.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define BUZZER_PIN 25 

// Buttons: Up (0), Down (1), Left (2), Right (3), Play (4)
const int btnPins[] = {13, 14, 27, 26, 32}; 
const int NUM_BTNS = 5;

enum AppMode { SOUND_ONLY, EMOJI_ONLY, BOTH };
enum Emotion { HAPPY, SAD, ANGRY, NEUTRAL };
enum MenuOption { OPTION_GAME, OPTION_SYNC };

// State Machine Configurations
AppMode currentMode = BOTH;
Emotion roundTarget = NEUTRAL;
MenuOption currentMenuSelection = OPTION_GAME; 
bool isRunning = false; 
bool isSyncActive = false; 

// Wi-Fi Credentials
const char* apSSID = "Toy-Data-Sync";
const char* apPassword = "password123"; 

WebServer server(80);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Visual Animation Pointers
const unsigned char* happyList[]   = { Irisoled::happy, Irisoled::excited, Irisoled::wink_left, Irisoled::wink_right };
const unsigned char* sadList[]     = { Irisoled::sad, Irisoled::worried, Irisoled::scared, Irisoled::despair }; 
const unsigned char* angryList[]   = { Irisoled::angry, Irisoled::furious, Irisoled::focused, Irisoled::warning };
const unsigned char* neutralList[] = { Irisoled::normal, Irisoled::bored, Irisoled::sleepy, Irisoled::disoriented };

const int happySize   = sizeof(happyList) / sizeof(happyList[0]);
const int sadSize     = sizeof(sadList) / sizeof(sadList[0]);
const int angrySize   = sizeof(angryList) / sizeof(angryList[0]);
const int neutralSize = sizeof(neutralList) / sizeof(neutralList[0]);

// Timers & Audio Variables
unsigned long audioPrevMillis = 0;
int audioStep = 0;
int audioDir = 1;
int audioFreq = 0;
int audioBurstCount = 0;
unsigned long animPrevMillis = 0;
int currentFrameIndex = 0;

// Analytical Matrix Registers (This Session)
unsigned long sessionStartTime = 0;
unsigned long roundStartTime = 0; 
int currentCorrect = 0;
int currentWrong = 0;
int totalQuestionsThisSession = 0;
unsigned long accumulatedCorrectTime = 0;
unsigned long accumulatedIncorrectTime = 0;

// Tracking position to handle row modifications seamlessly
long lastRowFilePosition = 0; 

void setup() {
    Serial.begin(115200);
    randomSeed(analogRead(34));

    for(int i=0; i<NUM_BTNS; i++) {
        pinMode(btnPins[i], INPUT_PULLUP);
    }

    ledcAttach(BUZZER_PIN, 1000, 8);
    ledcWrite(BUZZER_PIN, 0);

    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        for(;;);
    }

    // Initialize Internal SPIFFS Drive Partition
    if(!SPIFFS.begin(true)) {
        Serial.println("SPIFFS Mount Failed");
        for(;;);
    }

    // Initialize the structure layout of the file if it does not exist yet
    if(!SPIFFS.exists("/metrics.csv")) {
        File file = SPIFFS.open("/metrics.csv", FILE_WRITE);
        if(file) {
            file.println("Session_Duration_Ms,Total_Answers,Accuracy_Pct,Avg_Correct_Time_Ms,Avg_Incorrect_Time_Ms");
            file.close();
        }
    }

    renderMainMenu(); 
}

void loop() {
    if (isSyncActive) {
        server.handleClient();
    }

    if (isRunning) {
        runContinuousAudioEngine(roundTarget);
        runContinuousAnimationEngine(roundTarget);
    }
    
    handleInputs();
}

void renderMainMenu() {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(35, 10);
    display.print("CHOOSE MAP");

    if (currentMenuSelection == OPTION_GAME) {
        display.fillRect(10, 30, 50, 22, WHITE);
        display.setTextColor(BLACK);
    } else {
        display.drawRect(10, 30, 50, 22, WHITE);
        display.setTextColor(WHITE);
    }
    display.setCursor(22, 37); display.print("GAME");

    if (currentMenuSelection == OPTION_SYNC) {
        display.fillRect(68, 30, 50, 22, WHITE);
        display.setTextColor(BLACK);
    } else {
        display.drawRect(68, 30, 50, 22, WHITE);
        display.setTextColor(WHITE);
    }
    display.setCursor(80, 37); display.print("SYNC");
    display.display();
}

void renderSyncPortalScreen() {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.drawBitmap(0, 0, Irisoled::normal, 128, 64, WHITE);
    display.fillRect(0, 0, 128, 14, BLACK);
    display.setCursor(5, 3); display.print("LINK: 192.168.4.1");
    display.setCursor(20, 55); display.print("PRESS PLAY TO EXIT");
    display.display();
}

void startNewRound() {
    roundTarget = (Emotion)random(0, 4);
    audioStep = 0; audioDir = 1; audioBurstCount = 0; currentFrameIndex = 0;
    audioPrevMillis = millis(); animPrevMillis = millis();
    
    if (roundTarget == HAPPY)   audioFreq = 550;
    if (roundTarget == SAD)     audioFreq = 450;
    if (roundTarget == ANGRY)   audioFreq = 210;
    if (roundTarget == NEUTRAL) audioFreq = 300;

    roundStartTime = millis(); 
}

// Converts data to an optimized, strict formatting line string
String generateCSVLine() {
    unsigned long duration = millis() - sessionStartTime;
    float accuracy = 0.0;
    if(totalQuestionsThisSession > 0) {
        accuracy = ((float)currentCorrect / (float)totalQuestionsThisSession) * 100.0;
    }
    unsigned long avgCorrect = (currentCorrect > 0) ? (accumulatedCorrectTime / currentCorrect) : 0;
    unsigned long avgIncorrect = (currentWrong > 0) ? (accumulatedIncorrectTime / currentWrong) : 0;

    return String(duration) + "," + String(totalQuestionsThisSession) + "," + String((int)accuracy) + "," + String(avgCorrect) + "," + String(avgIncorrect);
}

// Commits or overwrites data lines cleanly inside internal Flash storage
void commitSessionLog(bool isModifyingLastLine) {
    String dataLine = generateCSVLine();

    if (isModifyingLastLine && lastRowFilePosition > 0) {
        // Dynamic seek modification: Open in read/write mode and truncate down to the last checkpoint anchor
        File file = SPIFFS.open("/metrics.csv", "r+");
        if(file) {
            file.seek(lastRowFilePosition);
            file.println(dataLine);
            file.close();
        }
    } else {
        // Fresh entry write loop
        File file = SPIFFS.open("/metrics.csv", FILE_APPEND);
        if(file) {
            lastRowFilePosition = file.position(); // Save position anchor to modify later if needed
            file.println(dataLine);
            file.close();
        }
    }
}

void handleInputs() {
    for(int i = 0; i < NUM_BTNS; i++) {
        if(digitalRead(btnPins[i]) == LOW) {
            delay(20); 
            if(digitalRead(btnPins[i]) == LOW) {
                
                // === MENU AND NAVIGATION LOGIC ===
                if (!isRunning && !isSyncActive) {
                    if (i == 2) { currentMenuSelection = OPTION_GAME; renderMainMenu(); }
                    if (i == 3) { currentMenuSelection = OPTION_SYNC; renderMainMenu(); }
                    if (i == 4) { 
                        if (currentMenuSelection == OPTION_GAME) {
                            isRunning = true;
                            totalQuestionsThisSession = 0;
                            currentCorrect = 0; currentWrong = 0;
                            accumulatedCorrectTime = 0; accumulatedIncorrectTime = 0;
                            lastRowFilePosition = 0;
                            sessionStartTime = millis(); // Lock session timestamp entry
                            startNewRound();
                        } else {
                            WiFi.softAP(apSSID, apPassword);
                            server.on("/", handleWebInterface);
                            server.on("/data.csv", handleCSVDownload);
                            server.on("/clear", handleWipeData);
                            server.begin();
                            isSyncActive = true;
                            renderSyncPortalScreen();
                        }
                    }
                    while(digitalRead(btnPins[i]) == LOW);
                    return;
                }

                // === RUNTIME EVALUATION LOGIC ===
                if (i == 4) { // PLAY Button: Explicit Session End Triggers File Closeout
                    ledcWrite(BUZZER_PIN, 0);
                    if (isRunning) {
                        isRunning = false;
                        if(totalQuestionsThisSession > 0) {
                            // Run immediate filesystem overwrite loop to guarantee metrics drop
                            commitSessionLog(totalQuestionsThisSession % 5 != 1 && totalQuestionsThisSession > 5);
                        }
                    }
                    if (isSyncActive) {
                        isSyncActive = false;
                        WiFi.softAPdisconnect(true);
                    }
                    renderMainMenu();
                    while(digitalRead(btnPins[i]) == LOW); 
                    return;
                }
                
                if (isRunning) {
                    unsigned long responseDuration = millis() - roundStartTime; 
                    Emotion choice = NEUTRAL;
                    if(i == 0) choice = HAPPY;
                    if(i == 1) choice = SAD;
                    if(i == 2) choice = ANGRY;
                    if(i == 3) choice = NEUTRAL;

                    ledcWrite(BUZZER_PIN, 0); 
                    display.clearDisplay();
                    
                    bool wasCorrect = (choice == roundTarget);
                    totalQuestionsThisSession++;

                    if(wasCorrect) {
                        currentCorrect++;
                        accumulatedCorrectTime += responseDuration;
                        display.drawTriangle(15, 35, 30, 50, 75, 15, WHITE); 
                        display.drawTriangle(15, 36, 30, 51, 75, 16, WHITE); 
                        ledcWriteTone(BUZZER_PIN, 600); ledcWrite(BUZZER_PIN, 10); delay(150);
                        ledcWriteTone(BUZZER_PIN, 900); delay(200);
                    } else {
                        currentWrong++;
                        accumulatedIncorrectTime += responseDuration;
                        display.drawLine(19, 12, 69, 52, WHITE);
                        display.drawLine(20, 12, 70, 52, WHITE); 
                        display.drawLine(69, 12, 19, 52, WHITE);
                        display.drawLine(70, 12, 20, 52, WHITE); 
                        ledcWriteTone(BUZZER_PIN, 140); ledcWrite(BUZZER_PIN, 12); delay(300);
                    }
                    
                    ledcWrite(BUZZER_PIN, 0); 

                    // HUD Setup
                    display.setTextSize(1);
                    display.setCursor(85, 15); display.print("OK: "); display.print(currentCorrect);
                    display.setCursor(85, 35); display.print("X:  "); display.print(currentWrong);
                    float accuracy = ((float)currentCorrect / (float)totalQuestionsThisSession) * 100.0;
                    display.setCursor(85, 52); display.print((int)accuracy); display.print("%");
                    display.display();
                    
                    // Paced Write Engine Rule: Every 5th question saves.
                    // If it's the first batch of 5, create a new line. After that, modify the last line.
                    if (totalQuestionsThisSession % 5 == 0) {
                        bool modifyMode = (totalQuestionsThisSession > 5);
                        commitSessionLog(modifyMode);
                    }

                    delay(3000); 
                    while(digitalRead(btnPins[i]) == LOW); 
                    startNewRound(); 
                }
            }
        }
    }
}

// Server Dashboard Panel UI
void handleWebInterface() {
    String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>body{font-family:sans-serif; background:#f4f6f9; padding:30px; text-align:center;}";
    html += ".container{max-width:500px; margin:auto; background:white; padding:30px; border-radius:12px; box-shadow:0 4px 15px rgba(0,0,0,0.05);}";
    html += "h1{color:#2c3e50;} .btn{display:block; padding:14px; margin:15px 0; font-size:16px; font-weight:bold; text-decoration:none; border-radius:8px; text-align:center;}";
    html += ".download{background:#3498db; color:white;} .clear{background:#e74c3c; color:white;}</style></head><body>";
    html += "<div class='container'><h1>Toy Data Center</h1><p>Select an option below to fetch session data logs directly from the device:</p>";
    html += "<a href='/data.csv' class='btn download'>Download CSV Log Data File</a>";
    html += "<a href='/clear' class='btn clear' onclick=\"return confirm('Permanently wipe file systems?');\">Format Storage Drive</a>";
    html += "</div></body></html>";
    server.send(200, "text/html", html);
}

// Directly streams the real physical file from flash memory over the airwaves
void handleCSVDownload() {
    File file = SPIFFS.open("/metrics.csv", "r");
    if (!file) {
        server.send(500, "text/plain", "File Missing");
        return;
    }
    // Set headers to force the browser to treat it as an Excel spreadsheet file download
    server.streamFile(file, "text/csv");
    file.close();
}

void handleWipeData() {
    SPIFFS.remove("/metrics.csv");
    File file = SPIFFS.open("/metrics.csv", FILE_WRITE);
    if(file) {
        file.println("Session_Duration_Ms,Total_Answers,Accuracy_Pct,Avg_Correct_Time_Ms,Avg_Incorrect_Time_Ms");
        file.close();
    }
    String html = "<html><head><script>alert('CSV wiped successfully!'); window.location.href='/';</script></head></html>";
    server.send(200, "text/html", html);
}

void runContinuousAnimationEngine(Emotion e) {
    if (currentMode == SOUND_ONLY) { display.clearDisplay(); display.display(); return; }
    unsigned long currentMillis = millis();
    if (currentMillis - animPrevMillis >= 400) { 
        animPrevMillis = currentMillis; display.clearDisplay();
        switch(e) {
            case HAPPY:   display.drawBitmap(0, 0, happyList[currentFrameIndex % happySize], 128, 64, WHITE); break;
            case SAD:     display.drawBitmap(0, 0, sadList[currentFrameIndex % sadSize], 128, 64, WHITE); break;
            case ANGRY:   display.drawBitmap(0, 0, angryList[currentFrameIndex % angrySize], 128, 64, WHITE); break;
            case NEUTRAL: display.drawBitmap(0, 0, neutralList[currentFrameIndex % neutralSize], 128, 64, WHITE); break;
        }
        display.display(); currentFrameIndex++;
    }
}

void runContinuousAudioEngine(Emotion e) {
    if (currentMode == EMOJI_ONLY) { ledcWrite(BUZZER_PIN, 0); return; }
    unsigned long currentMillis = millis();
    switch(e) {
        case HAPPY:
            if (currentMillis - audioPrevMillis >= 6) {
                audioPrevMillis = currentMillis; audioFreq += (40 * audioDir);
                if (audioFreq >= 850 || audioFreq <= 550) { audioDir *= -1; audioBurstCount++; }
                if ((audioBurstCount % 2) == 0) { ledcWriteTone(BUZZER_PIN, audioFreq); ledcWrite(BUZZER_PIN, 10); } 
                else { ledcWrite(BUZZER_PIN, 0); }
            }
            break;
        case SAD:
            if (currentMillis - audioPrevMillis >= 15) {
                audioPrevMillis = currentMillis; audioFreq -= 3;
                if (audioFreq < 220) audioFreq = 450; 
                ledcWriteTone(BUZZER_PIN, audioFreq); ledcWrite(BUZZER_PIN, 3);
            }
            break;
        case ANGRY:
            if (currentMillis - audioPrevMillis >= 100) {
                audioPrevMillis = currentMillis; audioFreq = (audioFreq == 210) ? 170 : 210; 
                ledcWriteTone(BUZZER_PIN, audioFreq); ledcWrite(BUZZER_PIN, 12);
            }
            break;
        case NEUTRAL:
            if (currentMillis - audioPrevMillis >= 25) {
                audioPrevMillis = currentMillis; audioFreq += (2 * audioDir);
                if (audioFreq >= 380 || audioFreq <= 300) audioDir *= -1;
                ledcWriteTone(BUZZER_PIN, audioFreq); ledcWrite(BUZZER_PIN, 3);
            }
            break;
    }
}
