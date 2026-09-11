#include <WiFi.h>
#include <LittleFS.h>

#define ST_SSID "YOUR_WIFI_SSID"
#define ST_PASS "YOUR_WIFI_PASSWORD"

WiFiServer server(80);

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== ESP32-S3 Starting ===");
  
  // Initialize LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Mount Failed - will format");
    if (!LittleFS.begin(true)) {
      Serial.println("LittleFS Format Failed!");
    } else {
      Serial.println("LittleFS Formatted and Mounted");
    }
  } else {
    Serial.println("LittleFS Mounted Successfully");
  }
  
  Serial.printf("Total space: %d bytes\n", LittleFS.totalBytes());
  Serial.printf("Used space: %d bytes\n", LittleFS.usedBytes());
  
  // Create a test file if none exist
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  if (!file) {
    Serial.println("Creating test file...");
    File testFile = LittleFS.open("/test.txt", "w");
    if (testFile) {
      testFile.println("Hello from ESP32-S3!");
      testFile.close();
      Serial.println("Test file created");
    }
  }
  
  // Disable power saving
  WiFi.setSleep(false);
  
  // Connect to WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ST_SSID, ST_PASS);
  
  Serial.print("Connecting to WiFi");
  int wifi_retry = 0;
  while (WiFi.status() != WL_CONNECTED && wifi_retry < 50) {
    delay(500);
    Serial.print(".");
    wifi_retry++;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi connection failed! Restarting...");
    delay(3000);
    ESP.restart();
  }
  
  Serial.println("\nWiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  Serial.print("Signal Strength: ");
  Serial.println(WiFi.RSSI());
  
  // Start server
  server.begin();
  Serial.println("Web server started!");
  Serial.println("=== Setup Complete ===\n");
}

void sendHomePage(WiFiClient &client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
  
  client.println("<!DOCTYPE html><html><head>");
  client.println("<meta name='viewport' content='width=device-width, initial-scale=1'>");
  client.println("<style>");
  client.println("body{font-family:Arial;margin:20px;background:#f0f0f0;}");
  client.println(".container{max-width:800px;margin:0 auto;background:white;padding:20px;border-radius:8px;}");
  client.println("h1{color:#333;border-bottom:2px solid #4CAF50;padding-bottom:10px;}");
  client.println("h2{color:#555;margin-top:30px;}");
  client.println(".stat{background:#e8f5e9;padding:10px;margin:5px 0;border-radius:4px;}");
  client.println(".file{background:#fff3e0;padding:8px;margin:5px 0;border-radius:4px;font-family:monospace;}");
  client.println("a{color:#2196F3;text-decoration:none;}");
  client.println("a:hover{text-decoration:underline;}");
  client.println(".nav{margin:20px 0;padding:10px;background:#e3f2fd;border-radius:4px;}");
  client.println("</style>");
  client.println("</head><body>");
  client.println("<div class='container'>");
  client.println("<h1>ESP32-S3 File Manager</h1>");
  
  client.println("<div class='nav'>");
  client.println("<a href='/'>Home</a> | ");
  client.println("<a href='/files'>File List</a> | ");
  client.println("<a href='/storage'>Storage Info</a>");
  client.println("</div>");
  
  client.println("<h2>System Status</h2>");
  client.print("<div class='stat'>Free Heap: ");
  client.print(ESP.getFreeHeap());
  client.println(" bytes</div>");
  
  client.print("<div class='stat'>WiFi RSSI: ");
  client.print(WiFi.RSSI());
  client.println(" dBm</div>");
  
  client.print("<div class='stat'>Uptime: ");
  client.print(millis() / 1000);
  client.println(" seconds</div>");
  
  client.println("</div></body></html>");
}

void sendFilesPage(WiFiClient &client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
  
  client.println("<!DOCTYPE html><html><head>");
  client.println("<meta name='viewport' content='width=device-width, initial-scale=1'>");
  client.println("<style>");
  client.println("body{font-family:Arial;margin:20px;background:#f0f0f0;}");
  client.println(".container{max-width:800px;margin:0 auto;background:white;padding:20px;border-radius:8px;}");
  client.println("h1{color:#333;border-bottom:2px solid #4CAF50;padding-bottom:10px;}");
  client.println(".file{background:#fff3e0;padding:10px;margin:5px 0;border-radius:4px;display:flex;justify-content:space-between;}");
  client.println(".filename{font-family:monospace;font-weight:bold;}");
  client.println(".filesize{color:#666;}");
  client.println("a{color:#2196F3;text-decoration:none;}");
  client.println(".nav{margin:20px 0;padding:10px;background:#e3f2fd;border-radius:4px;}");
  client.println("</style>");
  client.println("</head><body>");
  client.println("<div class='container'>");
  client.println("<h1>Files on LittleFS</h1>");
  
  client.println("<div class='nav'>");
  client.println("<a href='/'>Home</a> | ");
  client.println("<a href='/files'>File List</a> | ");
  client.println("<a href='/storage'>Storage Info</a>");
  client.println("</div>");
  
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  
  int fileCount = 0;
  while (file) {
    if (!file.isDirectory()) {
      client.println("<div class='file'>");
      client.print("<span class='filename'>");
      client.print(file.name());
      client.print("</span>");
      client.print("<span class='filesize'>");
      client.print(file.size());
      client.println(" bytes</span>");
      client.println("</div>");
      fileCount++;
    }
    file = root.openNextFile();
  }
  
  if (fileCount == 0) {
    client.println("<p>No files found. Upload some files to see them here!</p>");
  }
  
  client.println("</div></body></html>");
}

void sendStoragePage(WiFiClient &client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
  
  client.println("<!DOCTYPE html><html><head>");
  client.println("<meta name='viewport' content='width=device-width, initial-scale=1'>");
  client.println("<style>");
  client.println("body{font-family:Arial;margin:20px;background:#f0f0f0;}");
  client.println(".container{max-width:800px;margin:0 auto;background:white;padding:20px;border-radius:8px;}");
  client.println("h1{color:#333;border-bottom:2px solid #4CAF50;padding-bottom:10px;}");
  client.println(".stat{background:#e8f5e9;padding:15px;margin:10px 0;border-radius:4px;font-size:18px;}");
  client.println(".bar{background:#ddd;height:30px;border-radius:4px;overflow:hidden;margin:20px 0;}");
  client.println(".bar-fill{background:#4CAF50;height:100%;transition:width 0.3s;}");
  client.println("a{color:#2196F3;text-decoration:none;}");
  client.println(".nav{margin:20px 0;padding:10px;background:#e3f2fd;border-radius:4px;}");
  client.println("</style>");
  client.println("</head><body>");
  client.println("<div class='container'>");
  client.println("<h1>Storage Information</h1>");
  
  client.println("<div class='nav'>");
  client.println("<a href='/'>Home</a> | ");
  client.println("<a href='/files'>File List</a> | ");
  client.println("<a href='/storage'>Storage Info</a>");
  client.println("</div>");
  
  size_t totalBytes = LittleFS.totalBytes();
  size_t usedBytes = LittleFS.usedBytes();
  size_t freeBytes = totalBytes - usedBytes;
  int usedPercent = (usedBytes * 100) / totalBytes;
  
  client.print("<div class='stat'>Total Space: ");
  client.print(totalBytes);
  client.println(" bytes</div>");
  
  client.print("<div class='stat'>Used Space: ");
  client.print(usedBytes);
  client.print(" bytes (");
  client.print(usedPercent);
  client.println("%)</div>");
  
  client.print("<div class='stat'>Free Space: ");
  client.print(freeBytes);
  client.println(" bytes</div>");
  
  client.println("<div class='bar'>");
  client.print("<div class='bar-fill' style='width:");
  client.print(usedPercent);
  client.println("%'></div>");
  client.println("</div>");
  
  client.println("</div></body></html>");
}

void loop() {
  // Check for incoming clients
  WiFiClient client = server.available();
  
  if (client) {
    Serial.println("New client connected");
    String request = "";
    String firstLine = "";
    
    // Read the request
    while (client.connected()) {
      if (client.available()) {
        char c = client.read();
        request += c;
        
        // Capture first line
        if (firstLine.length() == 0 && c == '\n') {
          firstLine = request;
        }
        
        // If we've reached the end of the HTTP request headers
        if (request.endsWith("\r\n\r\n")) {
          break;
        }
      }
    }
    
    Serial.println("Request:");
    Serial.println(firstLine);
    
    // Route the request
    if (request.indexOf("GET / ") >= 0) {
      sendHomePage(client);
    } 
    else if (request.indexOf("GET /files") >= 0) {
      sendFilesPage(client);
    }
    else if (request.indexOf("GET /storage") >= 0) {
      sendStoragePage(client);
    }
    else {
      // 404 Not Found
      client.println("HTTP/1.1 404 Not Found");
      client.println("Content-Type: text/plain");
      client.println("Connection: close");
      client.println();
      client.println("404 - Page Not Found");
    }
    
    // Close connection
    delay(10);
    client.stop();
    Serial.println("Client disconnected\n");
  }
  
  // Monitor WiFi connection
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 10000) {
    lastCheck = millis();
    
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected! Reconnecting...");
      WiFi.disconnect();
      WiFi.reconnect();
    } else {
      Serial.printf("WiFi OK | RSSI: %d dBm | Heap: %d bytes\n", 
                    WiFi.RSSI(), ESP.getFreeHeap());
    }
  }
  
  delay(10);
}