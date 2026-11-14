# Automotive Safety Disclaimer & Limitation of Liability

---

> [!CAUTION]
> **FOR EDUCATIONAL, RESEARCH, AND HOBBYIST EXPERIMENTATION PURPOSES ONLY**
> This repository (including embedded firmware, diagnostic protocol stacks, ECU simulator software, host scripts, and documentation) is provided free of charge for educational, research, and hobbyist experimentation purposes only.

### 1. Automotive Multiplexing & Electrical Safety Warnings
Running experimental firmware connected to a motor vehicle's On-Board Diagnostics port (SAE J1962 / OBD-II) carries inherent operational and electrical hazards:
* **Bus Disturbance & Active Control:** Unintended CAN/TWAI or K-Line frames, timing errors, or bidirectional actuator commands (e.g., KWP2000 I/O control) may interfere with critical vehicle control units (ECU, TCM, ABS/ESP, instrument cluster, airbags, power steering).
* **Electrical & Ground Loop Hazards:** Connecting USB-C (`J2`) to an AC-grounded laptop while plugged into a live vehicle can cause destructive ground loop currents. Always follow the battery-only laptop rule and the anti-backfeeding bench wiring order documented in [README.md](README.md).
* **Regulatory & Warranty Implications:** Using uncertified diagnostic firmware or equipment may void manufacturer vehicle warranties or violate local road safety and vehicle inspection regulations.

### 2. Disclaimer of Warranties and Limitation of Liability
* **"AS IS" BASIS:** THE FIRMWARE, SOFTWARE TOOLS, AND DOCUMENTATION ARE PROVIDED ON AN "AS IS" AND "AS AVAILABLE" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING, WITHOUT LIMITATION, ANY WARRANTIES OR CONDITIONS OF TITLE, NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
* **NO LIABILITY:** IN NO EVENT SHALL THE AUTHORS, COPYRIGHT HOLDERS, OR CONTRIBUTORS BE LIABLE FOR ANY CLAIM, DAMAGES, OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT (INCLUDING NEGLIGENCE), OR OTHERWISE, ARISING FROM, OUT OF, OR IN CONNECTION WITH THE FIRMWARE, FLASHING, BENCH TESTING, VEHICULAR INTERFACING, OR THE INABILITY TO USE THE SOFTWARE DESCRIBED IN THIS REPOSITORY. THIS INCLUDES, WITHOUT LIMITATION, VEHICULAR DAMAGE, ELECTRONIC SYSTEM MALFUNCTION, ACCIDENTS, BODILY INJURY, LOSS OF LIFE, OR MONETARY LOSS.
* **USER SOLE RESPONSIBILITY:** ANY COMPILATION, FLASHING, BENCH POWERING, ROAD TESTING, AND VEHICLE CONNECTION IS UNDERTAKEN AT THE SOLE RISK AND DISCRETION OF THE USER. THE USER IS SOLELY RESPONSIBLE FOR COMPLYING WITH APPLICABLE TRANSPORT REGULATIONS AND MUST NEVER INTERACT WITH DIAGNOSTIC EQUIPMENT WHILE DRIVING WITHOUT A DEDICATED PASSENGER OPERATOR.
