import os
import shutil
from tkinter import Tk, Label, Button, filedialog, Frame, Toplevel
from PIL import Image, ImageTk

class ImageSorter:
    '''
    Simple application that allows you to set a parent directory, reads all images in the parent directory, and allows you to sort them into one of two
    other directories with left and right arrow keys.
    '''
    def __init__(self):
        self.root = Tk()
        self.root.title("Image Sorter")

        self.frame = Frame(self.root)
        self.frame.pack(pady=20)

        self.button_src = Button(self.frame, text="Select Source Directory", command=self.select_src_directory)
        self.button_src.pack(pady=10)

        self.button_eligible = Button(self.frame, text="Select Destination Directory 1", command=self.select_eligible_directory)
        self.button_eligible.pack(pady=10)

        self.button_ineligible = Button(self.frame, text="Select Destination Directory 2", command=self.select_ineligible_directory)
        self.button_ineligible.pack(pady=10)

        self.label = Label(self.frame)
        self.label.pack()
        self.label.bind("<Button-1>", self.show_full_image)

        self.root.bind("<Left>", self.mark_ineligible)
        self.root.bind("<Right>", self.mark_eligible)
        self.root.bind("<Down>", self.show_full_image)
        self.root.bind("<Up>", self.close_full_image)

        self.src_dir = ""
        self.eligible_dir = ""
        self.ineligible_dir = ""
        self.images = []
        self.index = 0
        self.full_img_window = None

        self.root.mainloop()

    def select_src_directory(self):
        self.src_dir = filedialog.askdirectory(title="Select Source Directory")
        if self.src_dir:
            self.images = [f for f in os.listdir(self.src_dir) if os.path.isfile(os.path.join(self.src_dir, f))]
            self.index = 0
            self.show_image()

    def select_eligible_directory(self):
        self.eligible_dir = filedialog.askdirectory(title="Destination Directory 1")

    def select_ineligible_directory(self):
        self.ineligible_dir = filedialog.askdirectory(title="Destination Directory 2")

    def show_image(self):
        if self.index < len(self.images):
            img_path = os.path.join(self.src_dir, self.images[self.index])
            self.current_image_path = img_path
            img = Image.open(img_path)
            img.thumbnail((1000, 1000))  # Resize for display purposes
            photo = ImageTk.PhotoImage(img)
            self.label.config(image=photo)
            self.label.image = photo
        else:
            self.label.config(text="No more images.")

    def show_full_image(self, event=None):
        if self.index < len(self.images):
            img_path = os.path.join(self.src_dir, self.images[self.index])
            img = Image.open(img_path)
            if self.full_img_window is None or not self.full_img_window.winfo_exists():
                self.full_img_window = Toplevel(self.root)
                self.full_img_window.title("Full Size Image")
                self.full_img_label = Label(self.full_img_window)
                self.full_img_label.pack()
                self.full_img_window.bind("<Left>", self.mark_ineligible)
                self.full_img_window.bind("<Right>", self.mark_eligible)
                self.full_img_window.bind("<Down>", self.close_full_image)
                self.full_img_window.bind("<Up>", self.close_full_image)
            full_photo = ImageTk.PhotoImage(img)
            self.full_img_label.config(image=full_photo)
            self.full_img_label.image = full_photo

    def close_full_image(self, event=None):
        if self.full_img_window is not None and self.full_img_window.winfo_exists():
            self.full_img_window.destroy()

    def mark_eligible(self, event):
        self.move_image(self.eligible_dir)

    def mark_ineligible(self, event):
        self.move_image(self.ineligible_dir)

    def move_image(self, dest_dir):
        if self.index < len(self.images):
            if not dest_dir:
                self.label.config(text="Select the appropriate directory")
                return
            img_path = os.path.join(self.src_dir, self.images[self.index])
            shutil.move(img_path, dest_dir)
            self.index += 1
            self.show_image()
            if self.full_img_window is not None and self.full_img_window.winfo_exists():
                self.show_full_image()

if __name__ == "__main__":
    sorter = ImageSorter()
