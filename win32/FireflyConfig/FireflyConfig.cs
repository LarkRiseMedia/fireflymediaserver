using System;
using System.IO;
using System.Drawing;
using System.Collections;
using System.Threading;
using System.ComponentModel;
using System.Windows.Forms;
using System.Data;
using System.ServiceProcess;
using System.Runtime.InteropServices;
using System.Text;
using JCMLib;

namespace FireflyConfig
{
	/// <summary>
	/// Summary description for Form1.
	/// </summary>
	/// 


	public enum ServiceStatus
	{
		Unintialized,
		Running,
		Stopped,
		Unknown
	}
		
	public class FireflyConfig : System.Windows.Forms.Form
	{
		[DllImport("Kernel32.dll")]
		static extern IntPtr CreateFile(
			string filename,
			[MarshalAs(UnmanagedType.U4)]FileAccess fileaccess,
			[MarshalAs(UnmanagedType.U4)]FileShare fileshare,
			int securityattributes,
			[MarshalAs(UnmanagedType.U4)]FileMode creationdisposition,
			int flags,
			IntPtr template);
		[DllImport("Kernel32.dll")]
		static extern IntPtr CreateNamedPipe(
			string filename,
			int openmode,
			int pipemode,
			int maxinstances,
			int outbuffersize,
			int inbuffersize,
			uint defaulttimeout,
			int securityattributes);

		private System.Threading.Thread PipeThread;

		private System.Drawing.Icon icnRunning;
		private System.Drawing.Icon icnStopped;
		private System.Drawing.Icon icnUnknown;

		private System.Windows.Forms.Timer timerRefresh;
		
		private System.ServiceProcess.ServiceController scFirefly;
		private ServiceStatus iState = ServiceStatus.Unintialized;

		private string strPort;
		private string strServerName;
		private string strMusicDir;
		private string strPassword;

		private bool ForceExit = false;

		private System.IntPtr PipeHandle;
		private System.IO.FileStream mPipeStream;
		private byte[] PipeBuffer;
		
		private System.Windows.Forms.Label label1;
		private System.Windows.Forms.Label label2;
		private System.Windows.Forms.Label label3;
		private System.Windows.Forms.Label label4;
		private System.Windows.Forms.GroupBox groupBox1;
		private System.Windows.Forms.TextBox textBoxPort;
		private System.Windows.Forms.TextBox textBoxServerName;
		private System.Windows.Forms.TextBox textBoxMusicDir;
		private System.Windows.Forms.Button buttonBrowseDir;

		private System.Windows.Forms.MenuItem menuItemStart;
		private System.Windows.Forms.MenuItem menuItemStop;
		private System.Windows.Forms.MenuItem menuItemConfigure;
		private System.Windows.Forms.ContextMenu trayMenu;
		private NotifyIconEx notifyIcon;
		private System.Windows.Forms.FolderBrowserDialog folderBrowserDialog;
		private System.Windows.Forms.MenuItem menuItemExit;
		private System.Windows.Forms.Button buttonOK;
		private System.Windows.Forms.Button buttonCancel;
		private System.Windows.Forms.TextBox textBoxPassword;
		private System.Windows.Forms.CheckBox checkBoxPassword;
		private System.ComponentModel.IContainer components;

		protected override void WndProc(ref Message msg) 
		{
			if(msg.Msg == 0x11) // WM_QUERYENDSESSION
			{
				ForceExit = true;
			}
			base.WndProc(ref msg);
		}

		public void ServiceStatusUpdate() 
		{
			scFirefly.Refresh();
			ServiceStatus iOldState = iState;
			switch(scFirefly.Status) 
			{
				case System.ServiceProcess.ServiceControllerStatus.Stopped:
					iState = ServiceStatus.Stopped;
					break;
				case System.ServiceProcess.ServiceControllerStatus.Running:
					iState = ServiceStatus.Running;
					break;
				default:
					iState = ServiceStatus.Unknown;
					break;
			}

			if(iState != iOldState) {
				switch(iState) 
				{
					case ServiceStatus.Stopped:
						menuItemStart.Enabled = true;
						menuItemStop.Enabled = false;
						notifyIcon.Icon = icnStopped;
						notifyIcon.Text = "Firefly Media Server is stopped";
						notifyIcon.ShowBalloon("Firefly Server","Server is stopped",JCMLib.NotifyIconEx.NotifyInfoFlags.Info,5000);
						break;
					case ServiceStatus.Running:
						menuItemStart.Enabled = false;
						menuItemStop.Enabled = true;
						notifyIcon.Icon = icnRunning;
						notifyIcon.Text = "Firefly Media Server is running";
						notifyIcon.ShowBalloon("Firefly Server","Server is running",JCMLib.NotifyIconEx.NotifyInfoFlags.Info,5000);
						break;
					case ServiceStatus.Unknown:
						menuItemStart.Enabled = false;
						menuItemStop.Enabled = false;
						notifyIcon.Icon = icnUnknown;
						notifyIcon.Text = "Cannot connect to Firefly Media Server";
						notifyIcon.ShowBalloon("Firefly Server","Cannot contact media server",JCMLib.NotifyIconEx.NotifyInfoFlags.Warning,5000);
						break;
				}
			}
		}

		public FireflyConfig()
		{
			//
			// Required for Windows Form Designer support
			//
			InitializeComponent();

			//
			// TODO: Add any constructor code after InitializeComponent call
			//

			/* grab the icons */
			System.IO.Stream st;
			System.Reflection.Assembly a = System.Reflection.Assembly.GetExecutingAssembly();
			st = a.GetManifestResourceStream("FireflyConfig.ff_run.ico");
			icnRunning = new System.Drawing.Icon(st);

			st = a.GetManifestResourceStream("FireflyConfig.ff_stop.ico");
			icnStopped = new System.Drawing.Icon(st);

			st = a.GetManifestResourceStream("FireflyConfig.ff_unknown.ico");
			icnUnknown = new System.Drawing.Icon(st);

			/* grab the service handle */
			scFirefly = new System.ServiceProcess.ServiceController("Firefly Media Server");

			/* set up the poller for service state */
			timerRefresh = new System.Windows.Forms.Timer();
			timerRefresh.Interval = 5000;
			timerRefresh.Enabled=true;
			timerRefresh.Start();
			timerRefresh.Tick += new EventHandler(timerRefresh_Tick);

			this.Visible=false;

			/* start chewing on the pipe */
			/*
			PipeHandle = NamedPipeNative.CreateFile(
				@"\\." + @"\pipe\" + "firefly", 
				NamedPipeNative.GENERIC_READ | NamedPipeNative.GENERIC_WRITE, 
				0, 
				null, 
				NamedPipeNative.CREATE_ALWAYS,
				0x40000000, 
				0);
*/				
/*
			PipeHandle = CreateFile("\\\\.\\pipe\\firefly",
				FileAccess.ReadWrite, FileShare.ReadWrite,
				0, FileMode.Create,0x40000000,IntPtr.Zero);
*/
			PipeHandle = CreateNamedPipe("\\\\.\\pipe\\firefly",
				3,0,10,4096,4096,0xffffffff,0);
			mPipeStream = new System.IO.FileStream(PipeHandle, System.IO.FileAccess.ReadWrite);
			PipeBuffer = new byte[4096];

			PipeThread = new Thread(new ThreadStart(PipeThreadFunction));
			PipeThread.Start();

		}

		public void PipeThreadFunction() 
		{
			int bytesRead;
			while((bytesRead = mPipeStream.Read(PipeBuffer,0,4096)) > 0) 
			{
				MessageBox.Show("woot");
			}
		}

		/// <summary>
		/// Clean up any resources being used.
		/// </summary>
		protected override void Dispose( bool disposing )
		{
			if( disposing )
			{
				if (components != null) 
				{
					components.Dispose();
				}
			}
			base.Dispose( disposing );
		}

		protected void LoadIni() 
		{
			string path = Environment.CurrentDirectory + "\\mt-daapd.conf";
			IniFile ifConfig = new IniFile(path);

			strPort = ifConfig.IniReadValue("general","port");
			strServerName = ifConfig.IniReadValue("general","servername");
			strMusicDir = ifConfig.IniReadValue("general","mp3_dir");
			strPassword = ifConfig.IniReadValue("general","password");

			textBoxPort.Text = strPort;
			textBoxServerName.Text = strServerName;
			textBoxMusicDir.Text = strMusicDir;
			textBoxPassword.Text = strPassword;
			if((strPassword == "") || (strPassword == null))
			{
				checkBoxPassword.Checked = false;
				textBoxPassword.Enabled = false;
			} 
			else 
			{
				checkBoxPassword.Checked = true;
				textBoxPassword.Enabled = true;
			}
		}

		public void timerRefresh_Tick(object sender, EventArgs eArgs) 
		{
			if(sender == timerRefresh) 
			{
				ServiceStatusUpdate();
			}
		}

		#region Windows Form Designer generated code
		/// <summary>
		/// Required method for Designer support - do not modify
		/// the contents of this method with the code editor.
		/// </summary>
		private void InitializeComponent()
		{
			System.Resources.ResourceManager resources = new System.Resources.ResourceManager(typeof(FireflyConfig));
			this.label1 = new System.Windows.Forms.Label();
			this.label2 = new System.Windows.Forms.Label();
			this.label3 = new System.Windows.Forms.Label();
			this.label4 = new System.Windows.Forms.Label();
			this.checkBoxPassword = new System.Windows.Forms.CheckBox();
			this.groupBox1 = new System.Windows.Forms.GroupBox();
			this.textBoxPassword = new System.Windows.Forms.TextBox();
			this.textBoxPort = new System.Windows.Forms.TextBox();
			this.textBoxServerName = new System.Windows.Forms.TextBox();
			this.textBoxMusicDir = new System.Windows.Forms.TextBox();
			this.buttonBrowseDir = new System.Windows.Forms.Button();
			this.buttonOK = new System.Windows.Forms.Button();
			this.buttonCancel = new System.Windows.Forms.Button();
			this.trayMenu = new System.Windows.Forms.ContextMenu();
			this.menuItemConfigure = new System.Windows.Forms.MenuItem();
			this.menuItemStart = new System.Windows.Forms.MenuItem();
			this.menuItemStop = new System.Windows.Forms.MenuItem();
			this.menuItemExit = new System.Windows.Forms.MenuItem();
			this.notifyIcon = new JCMLib.NotifyIconEx();
			this.folderBrowserDialog = new System.Windows.Forms.FolderBrowserDialog();
			this.groupBox1.SuspendLayout();
			this.SuspendLayout();
			// 
			// label1
			// 
			this.label1.Location = new System.Drawing.Point(8, 18);
			this.label1.Name = "label1";
			this.label1.Size = new System.Drawing.Size(80, 16);
			this.label1.TabIndex = 0;
			this.label1.Text = "Port:";
			// 
			// label2
			// 
			this.label2.Location = new System.Drawing.Point(8, 79);
			this.label2.Name = "label2";
			this.label2.Size = new System.Drawing.Size(88, 23);
			this.label2.TabIndex = 4;
			this.label2.Text = "Music Directory:";
			// 
			// label3
			// 
			this.label3.Location = new System.Drawing.Point(8, 47);
			this.label3.Name = "label3";
			this.label3.Size = new System.Drawing.Size(80, 23);
			this.label3.TabIndex = 2;
			this.label3.Text = "Server Name:";
			// 
			// label4
			// 
			this.label4.Location = new System.Drawing.Point(16, 50);
			this.label4.Name = "label4";
			this.label4.Size = new System.Drawing.Size(56, 16);
			this.label4.TabIndex = 1;
			this.label4.Text = "Password:";
			// 
			// checkBoxPassword
			// 
			this.checkBoxPassword.Location = new System.Drawing.Point(16, 16);
			this.checkBoxPassword.Name = "checkBoxPassword";
			this.checkBoxPassword.Size = new System.Drawing.Size(184, 24);
			this.checkBoxPassword.TabIndex = 0;
			this.checkBoxPassword.Text = "Use Music Library Password";
			this.checkBoxPassword.CheckedChanged += new System.EventHandler(this.checkBoxPassword_CheckedChanged);
			// 
			// groupBox1
			// 
			this.groupBox1.Controls.Add(this.textBoxPassword);
			this.groupBox1.Controls.Add(this.checkBoxPassword);
			this.groupBox1.Controls.Add(this.label4);
			this.groupBox1.Location = new System.Drawing.Point(8, 112);
			this.groupBox1.Name = "groupBox1";
			this.groupBox1.Size = new System.Drawing.Size(240, 80);
			this.groupBox1.TabIndex = 7;
			this.groupBox1.TabStop = false;
			this.groupBox1.Text = "Passsword";
			// 
			// textBoxPassword
			// 
			this.textBoxPassword.Location = new System.Drawing.Point(80, 48);
			this.textBoxPassword.Name = "textBoxPassword";
			this.textBoxPassword.Size = new System.Drawing.Size(144, 20);
			this.textBoxPassword.TabIndex = 2;
			this.textBoxPassword.Text = "";
			// 
			// textBoxPort
			// 
			this.textBoxPort.Location = new System.Drawing.Point(96, 16);
			this.textBoxPort.Name = "textBoxPort";
			this.textBoxPort.Size = new System.Drawing.Size(48, 20);
			this.textBoxPort.TabIndex = 1;
			this.textBoxPort.Text = "";
			// 
			// textBoxServerName
			// 
			this.textBoxServerName.Location = new System.Drawing.Point(96, 48);
			this.textBoxServerName.Name = "textBoxServerName";
			this.textBoxServerName.Size = new System.Drawing.Size(152, 20);
			this.textBoxServerName.TabIndex = 3;
			this.textBoxServerName.Text = "";
			// 
			// textBoxMusicDir
			// 
			this.textBoxMusicDir.Location = new System.Drawing.Point(96, 80);
			this.textBoxMusicDir.Name = "textBoxMusicDir";
			this.textBoxMusicDir.Size = new System.Drawing.Size(120, 20);
			this.textBoxMusicDir.TabIndex = 5;
			this.textBoxMusicDir.Text = "";
			// 
			// buttonBrowseDir
			// 
			this.buttonBrowseDir.Location = new System.Drawing.Point(224, 80);
			this.buttonBrowseDir.Name = "buttonBrowseDir";
			this.buttonBrowseDir.Size = new System.Drawing.Size(24, 20);
			this.buttonBrowseDir.TabIndex = 6;
			this.buttonBrowseDir.Text = "...";
			this.buttonBrowseDir.Click += new System.EventHandler(this.buttonBrowseDir_Click);
			// 
			// buttonOK
			// 
			this.buttonOK.DialogResult = System.Windows.Forms.DialogResult.OK;
			this.buttonOK.Location = new System.Drawing.Point(8, 216);
			this.buttonOK.Name = "buttonOK";
			this.buttonOK.Size = new System.Drawing.Size(80, 24);
			this.buttonOK.TabIndex = 8;
			this.buttonOK.Text = "&Ok";
			this.buttonOK.Click += new System.EventHandler(this.buttonOK_Click);
			// 
			// buttonCancel
			// 
			this.buttonCancel.DialogResult = System.Windows.Forms.DialogResult.Cancel;
			this.buttonCancel.Location = new System.Drawing.Point(168, 216);
			this.buttonCancel.Name = "buttonCancel";
			this.buttonCancel.Size = new System.Drawing.Size(80, 24);
			this.buttonCancel.TabIndex = 9;
			this.buttonCancel.Text = "&Cancel";
			this.buttonCancel.Click += new System.EventHandler(this.buttonCancel_Click);
			// 
			// trayMenu
			// 
			this.trayMenu.MenuItems.AddRange(new System.Windows.Forms.MenuItem[] {
																					 this.menuItemConfigure,
																					 this.menuItemStart,
																					 this.menuItemStop,
																					 this.menuItemExit});
			// 
			// menuItemConfigure
			// 
			this.menuItemConfigure.DefaultItem = true;
			this.menuItemConfigure.Index = 0;
			this.menuItemConfigure.Text = "&Configure Firefly Media Server...";
			this.menuItemConfigure.Click += new System.EventHandler(this.menuItemConfigure_Click);
			// 
			// menuItemStart
			// 
			this.menuItemStart.Enabled = false;
			this.menuItemStart.Index = 1;
			this.menuItemStart.Text = "S&tart Firefly Media Server";
			this.menuItemStart.Click += new System.EventHandler(this.menuItemStart_Click);
			// 
			// menuItemStop
			// 
			this.menuItemStop.Enabled = false;
			this.menuItemStop.Index = 2;
			this.menuItemStop.Text = "&Stop Firefly Media Server";
			this.menuItemStop.Click += new System.EventHandler(this.menuItemStop_Click);
			// 
			// menuItemExit
			// 
			this.menuItemExit.Index = 3;
			this.menuItemExit.Text = "E&xit";
			this.menuItemExit.Click += new System.EventHandler(this.menuItemExit_Click);
			// 
			// notifyIcon
			// 
			this.notifyIcon.ContextMenu = this.trayMenu;
			this.notifyIcon.Icon = ((System.Drawing.Icon)(resources.GetObject("notifyIcon.Icon")));
			this.notifyIcon.Text = "Firefly Media Server";
			this.notifyIcon.Visible = true;
			this.notifyIcon.DoubleClick += new System.EventHandler(this.notifyIcon_DoubleClick);
			// 
			// FireflyConfig
			// 
			this.AutoScaleBaseSize = new System.Drawing.Size(5, 13);
			this.ClientSize = new System.Drawing.Size(256, 246);
			this.ControlBox = false;
			this.Controls.Add(this.buttonCancel);
			this.Controls.Add(this.buttonOK);
			this.Controls.Add(this.buttonBrowseDir);
			this.Controls.Add(this.textBoxMusicDir);
			this.Controls.Add(this.textBoxServerName);
			this.Controls.Add(this.textBoxPort);
			this.Controls.Add(this.groupBox1);
			this.Controls.Add(this.label3);
			this.Controls.Add(this.label2);
			this.Controls.Add(this.label1);
			this.Icon = ((System.Drawing.Icon)(resources.GetObject("$this.Icon")));
			this.MaximizeBox = false;
			this.Name = "FireflyConfig";
			this.ShowInTaskbar = false;
			this.SizeGripStyle = System.Windows.Forms.SizeGripStyle.Hide;
			this.Text = "Configuration";
			this.WindowState = System.Windows.Forms.FormWindowState.Minimized;
			this.Resize += new System.EventHandler(this.FireflyConfig_Resize);
			this.Closing += new System.ComponentModel.CancelEventHandler(this.FireflyConfig_Closing);
			this.Load += new System.EventHandler(this.FireflyConfig_Load);
			this.groupBox1.ResumeLayout(false);
			this.ResumeLayout(false);

		}
		#endregion

		/// <summary>
		/// The main entry point for the application.
		/// </summary>
		[STAThread]
		static void Main() 
		{
			Application.Run(new FireflyConfig());
		}

		private void notifyIcon_DoubleClick(object sender, System.EventArgs e)
		{
			LoadIni();
			Show();
			WindowState = FormWindowState.Normal;
			ShowInTaskbar = true;	
		}

		private void FireflyConfig_Resize(object sender, System.EventArgs e)
		{
			if(FormWindowState.Minimized == WindowState) 
			{
				Hide();
			}
		}

		private void menuItemExit_Click(object sender, System.EventArgs e)
		{
			ForceExit = true;
			Close();
		}

		private void FireflyConfig_Load(object sender, System.EventArgs e)
		{
			Hide();
		}

		private void menuItemConfigure_Click(object sender, System.EventArgs e)
		{
			LoadIni();
			Show();
			WindowState = FormWindowState.Normal;
			ShowInTaskbar = true;	
		}

		private void menuItemStart_Click(object sender, System.EventArgs e)
		{
			scFirefly.Start();
		}

		private void menuItemStop_Click(object sender, System.EventArgs e)
		{
			scFirefly.Stop();
		}

		private void checkBoxPassword_CheckedChanged(object sender, System.EventArgs e)
		{
			textBoxPassword.Enabled = checkBoxPassword.Checked;
		}

		private void buttonCancel_Click(object sender, System.EventArgs e)
		{
			Hide();
		}

		private void buttonBrowseDir_Click(object sender, System.EventArgs e)
		{
			folderBrowserDialog.SelectedPath = textBoxMusicDir.Text;
			if(folderBrowserDialog.ShowDialog() == DialogResult.OK) 
			{
				textBoxMusicDir.Text = folderBrowserDialog.SelectedPath;
			}

		}

		private void buttonOK_Click(object sender, System.EventArgs e)
		{
			System.Windows.Forms.DialogResult result;
			bool changed=false;
			string strNewPassword;

			strNewPassword = textBoxPassword.Text;
			if(checkBoxPassword.Checked == false)
				strNewPassword = "";

			if(textBoxPort.Text != strPort)
				changed = true;
			if(textBoxServerName.Text != strServerName)
				changed = true;
			if(textBoxMusicDir.Text != strMusicDir)
				changed = true;
			if(strNewPassword != strPassword)
				changed = true;

			if(changed) 
			{
				result = MessageBox.Show("Changing these values will require a restart of the Firefly Media Server.  Are you sure you want to do this?",
					"Restart Confirmation",System.Windows.Forms.MessageBoxButtons.YesNoCancel);
				if(result == DialogResult.Yes) 
				{
					/* write the values */
					string path = Environment.CurrentDirectory + "\\mt-daapd.conf";
					IniFile ifConfig = new IniFile(path);

					ifConfig.IniWriteValue("general","port",textBoxPort.Text);
					ifConfig.IniWriteValue("general","servername",textBoxServerName.Text);
					ifConfig.IniWriteValue("general","mp3_dir",textBoxMusicDir.Text);
					ifConfig.IniWriteValue("general","password",strNewPassword);

					try 
					{
						timerRefresh.Enabled = false;
						notifyIcon.ShowBalloon("Firefly Server","Stopping Firefly Media Server",JCMLib.NotifyIconEx.NotifyInfoFlags.Info,5000);
						scFirefly.Stop();
						scFirefly.WaitForStatus(System.ServiceProcess.ServiceControllerStatus.Stopped,new TimeSpan(0,0,30));
						notifyIcon.ShowBalloon("Firefly Server","Starting Firefly Media Server",JCMLib.NotifyIconEx.NotifyInfoFlags.Info,5000);
						scFirefly.Start();
						notifyIcon.ShowBalloon("Firefly Server","Started Firefly Media Server",JCMLib.NotifyIconEx.NotifyInfoFlags.Info,5000);
						timerRefresh.Enabled = true;
					}
					catch (Exception ex) {
						ex = ex;
						MessageBox.Show("Error restarting server");
					};
					Hide();

				} 
				else if(result == DialogResult.No) 
				{
					Hide();
				} 
			} 
			else 
			{
				Hide();
			}
		}

		private void FireflyConfig_Closing(object sender, System.ComponentModel.CancelEventArgs e)
		{
			if(!ForceExit)
				e.Cancel = true;
		}

	}

	[StructLayout(LayoutKind.Sequential)]
	public class SecurityAttributes 
	{
	}


	public class NamedPipeNative
	{
		[DllImport("kernel32.dll", SetLastError=true)]
		public static extern IntPtr CreateFile(
			String lpFileName, // file name
			uint dwDesiredAccess, // access mode
			uint dwShareMode, // share mode
			SecurityAttributes attr, // SD
			uint dwCreationDisposition, // how to create
			uint dwFlagsAndAttributes, // file attributes
			uint hTemplateFile); // handle to template file


		public const uint GENERIC_READ = (0x80000000);
		public const uint GENERIC_WRITE = (0x40000000);
		public const uint CREATE_NEW = 1;
		public const uint CREATE_ALWAYS = 2;
		public const uint OPEN_EXISTING = 3;
	}

	public class IniFile
	{
		public string path;

		[DllImport("kernel32")]
		private static extern long WritePrivateProfileString(string section,
			string key,string val,string filePath);
		[DllImport("kernel32")]
		private static extern int GetPrivateProfileString(string section,
			string key,string def, StringBuilder retVal,
			int size,string filePath);

		/// <summary>
		/// INIFile Constructor.
		/// </summary>
		/// <PARAM name="INIPath"></PARAM>
		public IniFile(string INIPath)
		{
			path = INIPath;
		}
		/// <summary>
		/// Write Data to the INI File
		/// </summary>
		/// <PARAM name="Section"></PARAM>
		/// Section name
		/// <PARAM name="Key"></PARAM>
		/// Key Name
		/// <PARAM name="Value"></PARAM>
		/// Value Name
		public void IniWriteValue(string Section,string Key,string Value)
		{
			WritePrivateProfileString(Section,Key,Value,this.path);
		}
    
		/// <summary>
		/// Read Data Value From the Ini File
		/// </summary>
		/// <PARAM name="Section"></PARAM>
		/// <PARAM name="Key"></PARAM>
		/// <PARAM name="Path"></PARAM>
		/// <returns></returns>
		public string IniReadValue(string Section,string Key)
		{
			StringBuilder temp = new StringBuilder(4096);
			int i = GetPrivateProfileString(Section,Key,"",temp, 
				4096, this.path);
			return temp.ToString();

		}
	}

}
