# main.py
# pylint: disable=global-statement, unused-argument
from MicroWebSrv2 import MicroWebSrv2
from machine import Pin, ADC, PWM
myWebSockets = None 

def rotate(ang): 
    ang_2 = int(ang)*103/180+25 
    pwm = PWM(Pin(23))
    pwm.freq(50) 
    pwm.duty(int(ang_2))
    
def send_msg(msg):
    myWebSockets.SendTextMessage(msg) 

def OnWebSocketTextMsg(webSocket, msg):
    print('Servomotor position: {0}'.format(msg))
    rotate(msg) 
    send_msg(msg)

def OnWebSocketClosed(webSocket):
    global myWebSockets
    myWebSockets = None 

def OnWebSocketAccepted(microWebSrv2, webSocket):
    global myWebSockets
    if myWebSockets is None:
        print('WebSocket from {0}'.format(webSocket.Request.UserAddress))
        myWebSockets = webSocket
        myWebSockets.OnTextMessage = OnWebSocketTextMsg
        myWebSockets.OnClosed = OnWebSocketClosed

mws2 = MicroWebSrv2()
wsMod = MicroWebSrv2.LoadModule('WebSockets') 
wsMod.OnWebSocketAccepted = OnWebSocketAccepted
mws2.SetEmbeddedConfig()
mws2.NotFoundURL = '/'
mws2.StartManaged()

try:
    while True:
        continue
except KeyboardInterrupt:
    pass

mws2.Stop()     