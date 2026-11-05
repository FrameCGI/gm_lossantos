namespace winformsshit
{
    partial class Form1
    {
        /// <summary>
        ///  Required designer variable.
        /// </summary>
        private System.ComponentModel.IContainer components = null;

        /// <summary>
        ///  Clean up any resources being used.
        /// </summary>
        /// <param name="disposing">true if managed resources should be disposed; otherwise, false.</param>
        protected override void Dispose(bool disposing)
        {
            if (disposing && (components != null))
            {
                components.Dispose();
            }
            base.Dispose(disposing);
        }

        #region Windows Form Designer generated code

        /// <summary>
        ///  Required method for Designer support - do not modify
        ///  the contents of this method with the code editor.
        /// </summary>
        private void InitializeComponent()
        {
            pictureBox1 = new PictureBox();
            gmodPath = new TextBox();
            gmodPathBtn = new Button();
            label1 = new Label();
            label2 = new Label();
            gtaVPathBtn = new Button();
            gtaVPath = new TextBox();
            InstallBtn = new Button();
            progBar = new ProgressBar();
            consoleOutput = new TextBox();
            ((System.ComponentModel.ISupportInitialize)pictureBox1).BeginInit();
            SuspendLayout();
            // 
            // pictureBox1
            // 
            pictureBox1.BackgroundImage = Properties.Resources.icon;
            pictureBox1.BackgroundImageLayout = ImageLayout.Zoom;
            pictureBox1.Location = new Point(12, 12);
            pictureBox1.Name = "pictureBox1";
            pictureBox1.Size = new Size(434, 116);
            pictureBox1.TabIndex = 0;
            pictureBox1.TabStop = false;
            // 
            // gmodPath
            // 
            gmodPath.Location = new Point(116, 135);
            gmodPath.Name = "gmodPath";
            gmodPath.Size = new Size(267, 23);
            gmodPath.TabIndex = 1;
            // 
            // gmodPathBtn
            // 
            gmodPathBtn.Location = new Point(389, 134);
            gmodPathBtn.Name = "gmodPathBtn";
            gmodPathBtn.Size = new Size(57, 23);
            gmodPathBtn.TabIndex = 2;
            gmodPathBtn.Text = "Browse";
            gmodPathBtn.UseVisualStyleBackColor = true;
            // 
            // label1
            // 
            label1.AutoSize = true;
            label1.Location = new Point(12, 138);
            label1.Name = "label1";
            label1.Size = new Size(98, 15);
            label1.TabIndex = 3;
            label1.Text = "Garry's Mod Path";
            // 
            // label2
            // 
            label2.AutoSize = true;
            label2.Location = new Point(12, 176);
            label2.Name = "label2";
            label2.Size = new Size(65, 15);
            label2.TabIndex = 6;
            label2.Text = "GTA V Path";
            // 
            // gtaVPathBtn
            // 
            gtaVPathBtn.Location = new Point(389, 172);
            gtaVPathBtn.Name = "gtaVPathBtn";
            gtaVPathBtn.Size = new Size(57, 23);
            gtaVPathBtn.TabIndex = 5;
            gtaVPathBtn.Text = "Browse";
            gtaVPathBtn.UseVisualStyleBackColor = true;
            // 
            // gtaVPath
            // 
            gtaVPath.Location = new Point(116, 173);
            gtaVPath.Name = "gtaVPath";
            gtaVPath.Size = new Size(267, 23);
            gtaVPath.TabIndex = 4;
            // 
            // InstallBtn
            // 
            InstallBtn.Font = new Font("Segoe UI", 36F, FontStyle.Bold, GraphicsUnit.Point, 0);
            InstallBtn.ForeColor = Color.DodgerBlue;
            InstallBtn.Location = new Point(89, 300);
            InstallBtn.Name = "InstallBtn";
            InstallBtn.Size = new Size(294, 109);
            InstallBtn.TabIndex = 7;
            InstallBtn.Text = "Install";
            InstallBtn.UseVisualStyleBackColor = true;
            // 
            // progBar
            // 
            progBar.Location = new Point(-2, 427);
            progBar.Name = "progBar";
            progBar.Size = new Size(460, 23);
            progBar.TabIndex = 8;
            progBar.Value = 50;
            // 
            // consoleOutput
            // 
            consoleOutput.Location = new Point(16, 202);
            consoleOutput.Multiline = true;
            consoleOutput.Name = "consoleOutput";
            consoleOutput.ReadOnly = true;
            consoleOutput.Size = new Size(430, 92);
            consoleOutput.TabIndex = 9;
            // 
            // Form1
            // 
            AutoScaleDimensions = new SizeF(7F, 15F);
            AutoScaleMode = AutoScaleMode.Font;
            ClientSize = new Size(458, 450);
            Controls.Add(consoleOutput);
            Controls.Add(progBar);
            Controls.Add(InstallBtn);
            Controls.Add(label2);
            Controls.Add(gtaVPathBtn);
            Controls.Add(gtaVPath);
            Controls.Add(label1);
            Controls.Add(gmodPathBtn);
            Controls.Add(gmodPath);
            Controls.Add(pictureBox1);
            Name = "Form1";
            Text = "gm_lossantos Installer";
            ((System.ComponentModel.ISupportInitialize)pictureBox1).EndInit();
            ResumeLayout(false);
            PerformLayout();
        }

        #endregion

        private PictureBox pictureBox1;
        private TextBox gmodPath;
        private Button gmodPathBtn;
        private Label label1;
        private Label label2;
        private Button gtaVPathBtn;
        private TextBox gtaVPath;
        private Button InstallBtn;
        private ProgressBar progBar;
        private TextBox consoleOutput;
    }
}
