let lastMessage = "";

const floorNames = [
    "RDC",
    "Étage 1",
    "Étage 2",
    "Étage 3"
];

function setConnected(){
    const connection = document.getElementById("connection");
    connection.innerText = "● Connecté";
    connection.classList.remove("disconnected");
    connection.classList.add("connected");
}

function setDisconnected(){
    const connection = document.getElementById("connection");
    connection.innerText = "● Déconnecté";
    connection.classList.remove("connected");
    connection.classList.add("disconnected");
}

function toggleSettings(){
    document.getElementById("settingsPanel").classList.toggle("hidden");
}

function sendButton(floor, direction){
    const feedback = document.getElementById("feedback");
    feedback.innerText = "Envoi...";

    fetch(`/button?floor=${floor}&direction=${direction}`)
        .then(response => response.json())
        .then(data => {
            feedback.innerText = data.message || `Commande reçue (${floorNames[floor]} ${direction})`;
        })
        .catch(() => {
            feedback.innerText = "Erreur communication";
        });
}

function rfidMode(mode){
    const feedback = document.getElementById("feedback");
    feedback.innerText = "Mode RFID...";

    fetch(`/rfid?mode=${mode}`)
        .then(response => response.json())
        .then(data => {
            feedback.innerText = data.message || "Mode RFID changé";
        })
        .catch(() => {
            feedback.innerText = "Erreur RFID";
        });
}

function clearBadges(){
    if(!confirm("Effacer tous les badges enregistrés ?")) return;

    fetch("/badges/clear")
        .then(response => response.json())
        .then(data => {
            document.getElementById("feedback").innerText =
                data.message || "Badges supprimés";
        })
        .catch(() => {
            document.getElementById("feedback").innerText = "Erreur suppression badges";
        });
}

function saveCompensation(){
    const e1Up = document.getElementById("comp_e1_up").value || 0;
    const e1Down = document.getElementById("comp_e1_down").value || 0;
    const e2Up = document.getElementById("comp_e2_up").value || 0;
    const e2Down = document.getElementById("comp_e2_down").value || 0;

    const url =
        `/config/compensation?e1_up=${e1Up}&e1_down=${e1Down}&e2_up=${e2Up}&e2_down=${e2Down}`;

    fetch(url)
        .then(response => response.json())
        .then(data => {
            document.getElementById("feedback").innerText =
                data.message || "Compensation enregistrée";
        })
        .catch(() => {
            document.getElementById("feedback").innerText = "Erreur compensation";
        });
}

function updateCompInputs(data){
    if(!data.compensation) return;

    document.getElementById("comp_e1_up").value =
        data.compensation.e1_up ?? 0;

    document.getElementById("comp_e1_down").value =
        data.compensation.e1_down ?? 0;

    document.getElementById("comp_e2_up").value =
        data.compensation.e2_up ?? 0;

    document.getElementById("comp_e2_down").value =
        data.compensation.e2_down ?? 0;
}

function updateDisplay(floor, motor){
    for(let i = 0; i <= 3; i++){
        document.getElementById("green" + i).classList.remove("on");
        document.getElementById("red" + i).classList.remove("on");
        document.getElementById("cage" + i).classList.remove("active");

        if(i === floor){
            document.getElementById("green" + i).classList.add("on");
            document.getElementById("cage" + i).classList.add("active");
        }else{
            document.getElementById("red" + i).classList.add("on");
        }
    }

    document.getElementById("status").innerText =
        "Position : " + floorNames[floor];

    const motorLabel = document.getElementById("motor");
    motorLabel.classList.remove("running");
    motorLabel.classList.remove("stopped");

    if(motor){
        motorLabel.innerText = "● Moteur en marche";
        motorLabel.classList.add("running");
    }else{
        motorLabel.innerText = "● Moteur arrêté";
        motorLabel.classList.add("stopped");
    }
}

function setUnknownState(){
    for(let i = 0; i <= 3; i++){
        document.getElementById("green" + i).classList.remove("on");
        document.getElementById("red" + i).classList.add("on");
        document.getElementById("cage" + i).classList.remove("active");
    }

    document.getElementById("status").innerText = "Position : inconnue";

    const motorLabel = document.getElementById("motor");
    motorLabel.innerText = "● État moteur inconnu";
    motorLabel.classList.remove("running");
    motorLabel.classList.remove("stopped");
}

function updateRFID(data){
    document.getElementById("lastUid").innerText = data.dernierUID || "---";
    document.getElementById("rfidMode").innerText = data.modeRFID || "normal";
}

function updateFromESP32(){
    fetch("/status")
        .then(response => response.json())
        .then(data => {
            setConnected();

            const floor = data.position;

            if(floor >= 0 && floor <= 3){
                updateDisplay(floor, data.moteur);
            }else{
                setUnknownState();
            }

            updateRFID(data);
            updateCompInputs(data);

            if(data.message && data.message !== lastMessage){
                document.getElementById("feedback").innerText = data.message;
                lastMessage = data.message;
            }
        })
        .catch(() => {
            setDisconnected();
            setUnknownState();
        });
}

setDisconnected();
setUnknownState();

setInterval(updateFromESP32, 500);
updateFromESP32();
