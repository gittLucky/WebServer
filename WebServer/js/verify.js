var phoneReg = /^1[3|4|5|6|7|8]\d{9}$/;//手机号正则
var count = 60; //间隔函数，1秒执行
var InterValObj1; //timer变量，控制时间
var curCount1;//当前剩余秒数
var verify = "2580"
// 判断输入的手机号
function sendMessage1() {
    curCount1 = count;
    var phone = $.trim($('#phone1').val());
    if (!phoneReg.test(phone)) {
        alert(" 请输入有效的手机号码");
        return false;
    }
    //设置button效果，开始计时
    $("#mybtn").attr("disabled", "true");
    $("#mybtn").val(+ curCount1 + "秒再获取");
    InterValObj1 = window.setInterval(SetRemainTime1, 1000); //启动计时器，1秒执行一次
    //向后台发送处理数据

}

function SetRemainTime1() {
    if (curCount1 == 0) {
        window.clearInterval(InterValObj1);//停止计时器
        $("#mybtn").removeAttr("disabled");//启用按钮
        $("#mybtn").val("重新发送");
    }
    else {
        curCount1--;
        $("#mybtn").val(+ curCount1 + "秒再获取");
    }
}

// 提交
function binding() {
    var ver = document.getElementById("checkcode").value;
    if (ver == "" || ver == null) {
        alert("验证码不能为空!");
        return false;
    } else if (ver == verify) {
        window.location.href = "/home/student-4/wh/vscode-workspace/webServer/WebServer/doc/index.html";
        return true;
    } else {
        alert("验证码错误!");
        return false;
    }
}

// 处理 "返回首页" 按钮点击的函数
function goToHomePage() {
    // 默认会发送get请求
    window.location.href = "/home/student-4/wh/vscode-workspace/webServer/WebServer/doc/hello.html";
}